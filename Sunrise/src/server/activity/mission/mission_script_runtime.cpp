/**
 * The mission-program instance table, the durable state commit, the timers and the service slice.
 * The service slice and every lifecycle entry point take the mission runtime lock.
 */

#include "mission_script_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../host_runtime.h"
#include "mission_script_event_batch.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {

std::array<RuntimeInstance, host::kInstanceCapacity> g_instances{};

namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
bool g_enabled{};
bool g_pathReady{};

static_assert(mission_state::kSquadMemberCapacity == lua_vm::kSquadMemberCapacity);

/** Copies the complete VM outbox into typed State values in delivery order. */
[[nodiscard]] bool
snapshot_state_intents(const lua_vm::Vm& vm,
                       std::vector<mission_state::TypedIntent>& output) noexcept {
    return lua_vm::snapshot_intents(vm, output);
}

[[nodiscard]] bool earlier_timer(const lua_vm::MissionTimer& left,
                                 const lua_vm::MissionTimer& right) noexcept {
    return left.deadlineTick < right.deadlineTick
           || (left.deadlineTick == right.deadlineTick && left.sequence < right.sequence);
}

/** @return The life of the instance's own player, from its retained participation levels. */
[[nodiscard]] PlayerLife own_player_life(const RuntimeInstance& instance) noexcept {
    if (!instance.occupied
        || instance.playerLifeGeneration != instance.view.activityClientGeneration) {
        return PlayerLife::unknown;
    }
    for (const PlayerLifeObservation& life : instance.playerLife) {
        if (instance.playerKey != 0 && life.playerKey == instance.playerKey) {
            return life.life();
        }
    }
    return PlayerLife::unknown;
}

} // namespace

/**
 * Writes one bounded mission-script diagnostic line.
 * @param instance Binding the line reports, or null before one is bound.
 * @param fields Extra key=value pairs, appended as written.
 * @param error Free text; quoted and placed last so it never splits the pairs.
 */
void log_line(core::log::Level level,
              const RuntimeInstance* instance,
              std::string_view stage,
              std::string_view result,
              std::string_view fields,
              std::string_view error) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const unsigned long long session = instance == nullptr ? 0 : instance->view.binding.sessionId;
    const unsigned activityRow = instance == nullptr ? 0 : instance->identity.activityRow;
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=mission_script stage=%.*s result=%.*s session=%llu "
                                      "activity_row=%u",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      static_cast<int>(result.size()),
                                      result.data(),
                                      session,
                                      activityRow);
    if (written <= 0) {
        return;
    }
    std::size_t length = (std::min)(static_cast<std::size_t>(written), line.size() - 1);
    if (!fields.empty()) {
        const int piece = std::snprintf(line.data() + length,
                                        line.size() - length,
                                        " %.*s",
                                        static_cast<int>(fields.size()),
                                        fields.data());
        if (piece > 0) {
            length = (std::min)(length + static_cast<std::size_t>(piece), line.size() - 1);
        }
    }
    if (!error.empty()) {
        const int piece = std::snprintf(line.data() + length,
                                        line.size() - length,
                                        " error=\"%.*s\"",
                                        static_cast<int>(error.size()),
                                        error.data());
        if (piece > 0) {
            length = (std::min)(length + static_cast<std::size_t>(piece), line.size() - 1);
        }
    }
    core::log::write(core::log::Channel::server, level, {line.data(), length});
}

/**
 * Raises a fireteam event on each private instance whose party life counts changed. The party
 * is the committed destination peers; a missing or loading member counts as unknown.
 */
void publish_fireteam_life(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.publicTarget || !instance.sessionRosterObserved
            || instance.programStatus != ProgramStatus::loaded) {
            continue;
        }
        FireteamLife counts{};
        counts.add(own_player_life(instance));
        for (const SessionRosterWatch& peer : instance.sessionRoster) {
            if (!peer.used) {
                continue;
            }
            PlayerLife life = PlayerLife::unknown;
            for (const RuntimeInstance& candidate : g_instances) {
                if (candidate.occupied && !candidate.publicTarget
                    && candidate.view.binding.sessionId == peer.sessionId
                    && candidate.view.binding.createdRevision == peer.createdRevision) {
                    life = own_player_life(candidate);
                    break;
                }
            }
            counts.add(life);
        }
        if (instance.fireteamLifePublished && counts == instance.lastFireteamLife) {
            continue;
        }
        instance.lastFireteamLife = counts;
        instance.fireteamLifePublished = true;
        host::Event event{};
        event.kind = host::EventKind::fireteamState;
        event.binding = instance.view.binding;
        event.sequence = instance.missionStateRevision;
        event.sourceGeneration = instance.view.activityClientGeneration;
        event.missionSequence = instance.lastMissionSequence;
        event.tick = now;
        event.fireteamAlive = counts.alive;
        event.fireteamDead = counts.dead;
        event.fireteamUnknown = counts.unknown;
        push_script_event(instance, event);
        std::array<char, 96> fields{};
        const int length = std::snprintf(fields.data(),
                                         fields.size(),
                                         "alive=%u dead=%u unknown=%u",
                                         static_cast<unsigned>(counts.alive),
                                         static_cast<unsigned>(counts.dead),
                                         static_cast<unsigned>(counts.unknown));
        if (length > 0) {
            log_line(core::log::Level::debug,
                     &instance,
                     "fireteam_life",
                     "changed",
                     {fields.data(), static_cast<std::size_t>(length)});
        }
    }
}

/** Merges the type-13 participation records of one Sense snapshot into the instance. */
void observe_player_life(RuntimeInstance& instance,
                         const host::SenseObservationSnapshot& sense) noexcept {
    if (instance.playerLifeGeneration != sense.sourceGeneration) {
        instance.playerLife = {};
        instance.playerLifeGeneration = sense.sourceGeneration;
    }
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != kParticipationSlotType
            || observation.key.senseSchema != kParticipationSenseSchema
            || observation.key.objectTag != kParticipationObjectTag
            || observation.key.slotIndex < kFirstParticipationSlot
            || observation.key.slotIndex >= kFirstParticipationSlot + kParticipationSlotCount
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue) {
            continue;
        }
        update_player_life(
            instance.playerLife[observation.key.slotIndex - kFirstParticipationSlot],
            std::span(sense.values).subspan(observation.firstValue, observation.valueCount));
    }
}

/**
 * Appends one host-state event for the script.
 * Host-state bursts are intentionally dynamic: authored Sense updates can raise more events than
 * any fixed bound without making the events invalid.
 */
void push_script_event(RuntimeInstance& instance, const host::Event& event) noexcept {
    if (instance.programStatus != ProgramStatus::loaded
        || !lua_vm::handles_event(instance.vm, event.kind)) {
        return;
    }
    try {
        instance.scriptEvents.push_back(event);
    } catch (const std::bad_alloc&) {
        log_line(core::log::Level::warn, &instance, "script_event", "allocation_failure");
    }
}

/** Retains the last VM stage and status shown on the panel. */
void note_vm_status(RuntimeInstance& instance,
                    std::string_view stage,
                    std::string_view status) noexcept {
    copy_text(instance.lastVmStage, stage);
    copy_text(instance.lastVmStatus, status);
}

/** Frees one slot; its queued events are retired unless the caller keeps them for a reattach. */
void clear_instance(RuntimeInstance& instance, bool clearPending) noexcept {
    if (instance.occupied && clearPending) {
        server::gameplay::squad_entity_retirement::cancel_placed_transition(
            instance.view.binding, instance.view.activityClientGeneration);
        clear_pending_events(instance.view.binding);
    } else if (instance.occupied) {
        reset_pending_events_for_reattach(instance.view.binding);
    }
    lua_vm::close(instance.vm);
    instance.worldView = {};
    instance.view = {};
    instance.identity = {};
    instance.programKey = {};
    instance.lastVmStage = {};
    instance.lastVmStatus = {};
    instance.eventsSeen = 0;
    instance.eventsCommitted = 0;
    instance.intentsTransportStaged = 0;
    instance.lastEventSequence = 0;
    instance.lastLoggedRevision = 0;
    instance.lastMissionSequence = 0;
    instance.missionPhase = 0;
    instance.missionStateRevision = 0;
    instance.activityStateRevision = 0;
    instance.durableIntentSequence = 0;
    instance.durableHostOutputRevision = 0;
    instance.expectedScriptableRevision = 0;
    instance.deliveryDeadline = 0;
    instance.firstIntentAttempt = 0;
    instance.nextIntentAttempt = 0;
    instance.firstStartAttempt = 0;
    instance.nextStartAttempt = 0;
    instance.pendingTimerEvent = {};
    instance.firstTimerAttempt = 0;
    instance.nextTimerAttempt = 0;
    instance.intentAttempts = 0;
    instance.startAttempts = 0;
    instance.timerAttempts = 0;
    instance.durablePendingIntentCount = 0;
    instance.lastIntentStatus = (std::numeric_limits<std::uint16_t>::max)();
    instance.initialStateRegion = -1;
    instance.activeRegion = -1;
    instance.programStatus = ProgramStatus::none;
    instance.deliveryStage = DeliveryStage::idle;
    instance.publicTarget = false;
    instance.playerKey = 0;
    instance.missionStateBound = false;
    instance.missionStarted = false;
    instance.missionStateFaulted = false;
    instance.initialStateDeclared = false;
    instance.initialStateSelected = false;
    instance.missionReattached = false;
    instance.startPending = false;
    instance.timerPending = false;
    instance.triggerOccupancy = {};
    instance.ghostObservations = {};
    instance.actorPathObservations = {};
    instance.squadObservations = {};
    instance.sceneObservations = {};
    instance.objectiveObservations = {};
    instance.sessionRoster = {};
    instance.sessionRosterObserved = false;
    instance.playerLife = {};
    instance.playerLifeGeneration = 0;
    instance.lastFireteamLife = {};
    instance.fireteamLifePublished = false;
    std::vector<host::Event>{}.swap(instance.scriptEvents);
    instance.firstScriptEventAttempt = 0;
    instance.nextScriptEventAttempt = 0;
    instance.scriptEventAttempts = 0;
    instance.scriptEventRead = 0;
    instance.occupied = false;
}

/** @return The open slot for one exact binding, or null. */
RuntimeInstance* find_instance(const state::activity::SessionBinding& binding) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (instance.occupied && same_binding(instance.view.binding, binding)) {
            return &instance;
        }
    }
    return nullptr;
}

/** @return One unused slot, or null when every slot is open. */
RuntimeInstance* free_instance() noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied) {
            return &instance;
        }
    }
    return nullptr;
}

/** Copies one committed authoritative snapshot into the runtime's exact compare baseline. */
void accept_mission_state(RuntimeInstance& instance,
                          const mission_state::Snapshot& snapshot) noexcept {
    instance.missionStateRevision = snapshot.state.revision;
    instance.lastMissionSequence = snapshot.state.inputSequence;
    instance.activityStateRevision = snapshot.activityStateRevision;
    instance.missionPhase = snapshot.state.phase;
    instance.missionStarted = snapshot.state.started;
    instance.missionStateFaulted = snapshot.state.faulted;
    instance.durablePendingIntentCount = snapshot.state.pendingIntents.size();
    instance.durableIntentSequence = snapshot.state.pendingIntents.empty()
                                         ? mission_state::kAbsentIntentSequence
                                         : snapshot.state.pendingIntents.front().sequence;
    instance.durableHostOutputRevision =
        snapshot.state.pendingIntents.empty()
            ? mission_state::kAbsentHostOutputRevision
            : snapshot.state.pendingIntents.front().hostOutputRevision;
}

/** Marks the already-faulted VM in durable State when its exact compare still matches. */
void persist_mission_fault(RuntimeInstance& instance) noexcept {
    if (!instance.missionStateBound) {
        return;
    }
    mission_state::Snapshot snapshot{};
    const mission_state::Status status = mission_state::fault(
        instance.view.binding, instance.programKey, instance.missionStateRevision, snapshot);
    if (status == mission_state::Status::ready) {
        accept_mission_state(instance, snapshot);
        return;
    }
    log_line(core::log::Level::warn, &instance, "state_fault", mission_state::status_name(status));
}

/** Faults both the VM and the exact server-owned mission record. */
void fault_instance(RuntimeInstance& instance, std::string_view reason) noexcept {
    server::gameplay::squad_entity_retirement::cancel_placed_transition(
        instance.view.binding, instance.view.activityClientGeneration);
    lua_vm::fault(instance.vm, reason);
    instance.programStatus = ProgramStatus::programError;
    persist_mission_fault(instance);
}

/** Commits the VM's phase/revision/start transaction into exact server-owned State. */
bool commit_mission_state(RuntimeInstance& instance,
                          bool started,
                          std::uint64_t nextInputSequence) noexcept {
    if (!instance.missionStateBound) {
        fault_instance(instance, "mission program has no authoritative State binding");
        return false;
    }
    lua_vm::Snapshot vm{};
    lua_vm::snapshot(instance.vm, vm);
    std::vector<mission_state::TypedIntent> pendingIntents{};
    std::array<mission_state::ScriptVariable, mission_state::kVariableCapacity> variables{};
    std::array<mission_state::MissionTimer, mission_state::kTimerCapacity> timers{};
    std::size_t variableCount = 0;
    std::size_t timerCount = 0;
    std::uint64_t nextTimerSequence = mission_state::kAbsentTimerSequence;
    std::uint64_t nextIntentKey = mission_state::kAbsentIntentKey;
    if (!snapshot_state_intents(instance.vm, pendingIntents)) {
        fault_instance(instance, "mission VM outbox snapshot was refused");
        return false;
    }
    if (!lua_vm::snapshot_durable_state(instance.vm,
                                        variables,
                                        variableCount,
                                        timers,
                                        timerCount,
                                        nextTimerSequence,
                                        nextIntentKey)) {
        fault_instance(instance, "mission VM durable-state snapshot was refused");
        return false;
    }
    const mission_state::CommitCandidate transaction{
        .variables = {variables.data(), variableCount},
        .timers = {timers.data(), timerCount},
        .pendingIntents = pendingIntents,
        .nextRevision = vm.stateRevision,
        .nextInputSequence = nextInputSequence,
        .nextTimerSequence = nextTimerSequence,
        .nextIntentKey = nextIntentKey,
        .phase = vm.phase,
        .started = started,
    };
    mission_state::Snapshot snapshot{};
    const mission_state::Status status = mission_state::commit(instance.view.binding,
                                                               instance.programKey,
                                                               instance.missionStateRevision,
                                                               instance.lastMissionSequence,
                                                               transaction,
                                                               snapshot);
    if (status == mission_state::Status::ready) {
        const std::uint32_t previousPhase = instance.missionPhase;
        accept_mission_state(instance, snapshot);
        if (instance.missionPhase != previousPhase) {
            queue_phase_entered(instance, previousPhase);
        }
        return true;
    }
    log_line(core::log::Level::warn, &instance, "state_commit", mission_state::status_name(status));
    fault_instance(instance, "authoritative mission State commit was refused");
    instance.programStatus = ProgramStatus::programError;
    return false;
}

namespace {

/**
 * Settles the inputs of every instance whose activity has no script. No program will consume
 * them, and an unsettled row is retained by the Host feed until its capacity refuses new ones.
 */
void retire_scriptless_inputs() noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.programStatus != ProgramStatus::missing) {
            continue;
        }
        mission_state::InputSequenceSnapshot inputState{};
        if (!mission_state::input_sequence_snapshot(instance.view.binding, inputState)
            || inputState.issued == inputState.committed) {
            continue;
        }
        const mission_state::Status status =
            mission_state::retire_unbound_inputs(instance.view.binding);
        if (status != mission_state::Status::ready) {
            log_line(core::log::Level::warn,
                     &instance,
                     "events",
                     mission_state::status_name(status),
                     "reason=scriptless_inputs_retained");
        }
    }
}

/** Delivers queued derived events in arrival order until one needs output or the batch is spent. */
void service_script_events(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        const auto ready = [&] {
            if (!instance.occupied || instance.scriptEventRead >= instance.scriptEvents.size()
                || instance.programStatus != ProgramStatus::loaded || instance.startPending) {
                return false;
            }
            lua_vm::Intent pendingIntent{};
            return instance.deliveryStage == DeliveryStage::idle
                   && !lua_vm::pending_intent(instance.vm, pendingIntent);
        };
        const auto dispatch = [&] {
            const bool firstAttempt = instance.scriptEventAttempts == 0;
            if (firstAttempt) {
                instance.firstScriptEventAttempt = now;
            }
            ++instance.scriptEventAttempts;
            host::Event& head = instance.scriptEvents[instance.scriptEventRead];
            head.tick = now;
            static_cast<void>(dispatch_event(instance, head, nullptr, nullptr, firstAttempt, now));
            retire_script_event(instance);
        };
        static_cast<void>(drain_script_event_batch(ready, dispatch));
    }
}

/** Selects at most one due timer per instance without adding a second durable event feed. */
void service_timers(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || instance.programStatus != ProgramStatus::loaded
            || instance.startPending) {
            continue;
        }
        lua_vm::Intent pendingIntent{};
        if (instance.deliveryStage != DeliveryStage::idle
            || lua_vm::pending_intent(instance.vm, pendingIntent)) {
            continue;
        }
        if (!instance.timerPending) {
            mission_state::InputSequenceSnapshot inputState{};
            if (has_pending_host_input(instance)
                || !mission_state::input_sequence_snapshot(instance.view.binding, inputState)
                || inputState.faulted || inputState.issued != inputState.committed) {
                continue;
            }
            std::array<lua_vm::ScriptVariable, lua_vm::kVariableCapacity> variables{};
            std::array<lua_vm::MissionTimer, lua_vm::kTimerCapacity> timers{};
            std::size_t variableCount = 0;
            std::size_t timerCount = 0;
            std::uint64_t nextTimerSequence = mission_state::kAbsentTimerSequence;
            std::uint64_t nextIntentKey = mission_state::kAbsentIntentKey;
            if (!lua_vm::snapshot_durable_state(instance.vm,
                                                variables,
                                                variableCount,
                                                timers,
                                                timerCount,
                                                nextTimerSequence,
                                                nextIntentKey)) {
                fault_instance(instance, "mission timer snapshot was refused");
                continue;
            }
            const lua_vm::MissionTimer* selected = nullptr;
            for (std::size_t index = 0; index < timerCount; ++index) {
                if (timers[index].deadlineTick <= now
                    && (selected == nullptr || earlier_timer(timers[index], *selected))) {
                    selected = &timers[index];
                }
            }
            if (selected == nullptr) {
                continue;
            }
            instance.pendingTimerEvent = {};
            instance.pendingTimerEvent.binding = instance.view.binding;
            instance.pendingTimerEvent.timerName = selected->key;
            instance.pendingTimerEvent.sequence = selected->sequence;
            instance.pendingTimerEvent.tick = now;
            instance.pendingTimerEvent.sourceGeneration = instance.view.activityClientGeneration;
            instance.pendingTimerEvent.timerDeadlineTick = selected->deadlineTick;
            instance.pendingTimerEvent.timerSequence = selected->sequence;
            instance.pendingTimerEvent.missionSequence = instance.lastMissionSequence;
            instance.pendingTimerEvent.kind = host::EventKind::timerElapsed;
            instance.firstTimerAttempt = now;
            instance.nextTimerAttempt = now;
            instance.timerAttempts = 0;
            instance.timerPending = true;
        }
        const bool firstAttempt = instance.timerAttempts == 0;
        ++instance.timerAttempts;
        static_cast<void>(dispatch_event(
            instance, instance.pendingTimerEvent, nullptr, nullptr, firstAttempt, now));
        instance.pendingTimerEvent = {};
        instance.firstTimerAttempt = 0;
        instance.nextTimerAttempt = 0;
        instance.timerAttempts = 0;
        instance.timerPending = false;
    }
}

} // namespace

/** Clears all state, reads the enable switch, and resolves the script and SDK Lua paths. */
void initialize() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    clear_attach_diagnostics();
    clear_all_pending_events();
    for (RuntimeInstance& instance : g_instances) {
        clear_instance(instance);
    }
    g_enabled = core::settings::get().server.activation.missionScripting;
    g_pathReady = false;
    reset_feed_cursors();
    if (!g_enabled) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_pathReady = resolve_script_paths();
    if (!g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    log_line(core::log::Level::info, nullptr, "initialize", "enabled");
    ReleaseSRWLockExclusive(&g_lock);
}

/** One pass: attach, start, events, timers, then delivery for every open instance. */
void service(std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    synchronize_instances(now);
    service_pending_starts(now);
    consume_delivery_events(now);
    consume_mission_inputs(now);
    retire_scriptless_inputs();
    service_timers(now);
    service_script_events(now);
    for (RuntimeInstance& instance : g_instances) {
        if (instance.occupied) {
            if (instance.programStatus == ProgramStatus::programError) {
                reconcile_terminal_delivery(instance);
            } else {
                dispatch_intent(instance, now);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies every open instance row and every retained attach row for the panel. */
void snapshot(DiagnosticsSnapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    output.enabled = g_enabled;
    output.pathReady = g_pathReady;
    for (const RuntimeInstance& instance : g_instances) {
        if (instance.occupied && output.instanceCount < output.instances.size()) {
            copy_diagnostics(instance, output.instances[output.instanceCount++]);
        }
    }
    snapshot_attach_diagnostics(output);
    ReleaseSRWLockShared(&g_lock);
}

/** Closes every instance and authorizes one program replacement so the next attach recompiles. */
bool reload() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    bool reloaded = true;
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied) {
            continue;
        }
        log_line(core::log::Level::info, &instance, "reload", "requested");
        if (instance.missionStateBound && instance.missionStateFaulted) {
            mission_state::Snapshot recovered{};
            const mission_state::Status status =
                mission_state::recover(instance.view.binding,
                                       instance.programKey,
                                       instance.missionStateRevision,
                                       recovered);
            if (status != mission_state::Status::ready) {
                log_line(core::log::Level::warn,
                         &instance,
                         "reload",
                         mission_state::status_name(status));
                reloaded = false;
                continue;
            }
            accept_mission_state(instance, recovered);
            clear_pending_events(instance.view.binding);
        }
        if (!authorize_reload(instance)) {
            log_line(core::log::Level::warn, &instance, "reload", "capacity");
            reloaded = false;
            continue;
        }
        clear_instance(instance, false);
    }
    clear_attach_diagnostics();
    ReleaseSRWLockExclusive(&g_lock);
    return reloaded;
}

/** Closes every instance and clears every global this unit owns. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (RuntimeInstance& instance : g_instances) {
        clear_instance(instance);
    }
    clear_all_pending_events();
    clear_attach_diagnostics();
    clear_script_paths();
    clear_feed_cursors();
    g_enabled = false;
    g_pathReady = false;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::activity::mission
