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
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/external/external_entity_codec.h"
#include "../../../state/activity/entity_slots/member_leases.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../bap/runtime.h"
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

[[nodiscard]] bool earlier_timer(const lua_vm::MissionTimer& left,
                                 const lua_vm::MissionTimer& right) noexcept {
    return left.deadlineTick < right.deadlineTick
           || (left.deadlineTick == right.deadlineTick && left.sequence < right.sequence);
}

} // namespace

MemberInputState* member_input(RuntimeInstance& instance, std::uint64_t generation) noexcept {
    if (!generation) {
        return nullptr;
    }
    for (auto& member : instance.memberInputs) {
        if (member.present && member.life.sourceGeneration == generation) {
            return &member;
        }
    }
    return nullptr;
}

void refresh_mission_members(RuntimeInstance& instance) noexcept {
    namespace membership = state::activity::membership;
    membership::MissionParty party{};
    std::size_t count{};
    instance.partySnapshotValid =
        membership::snapshot_mission_party(instance.view.binding, party, count);
    try {
        if (instance.memberInputs.size() < count) {
            instance.memberInputs.resize(count);
        }
    } catch (const std::bad_alloc&) {
        instance.partySnapshotValid = false;
        for (auto& member : instance.memberInputs) {
            member.present = false;
        }
        return;
    }
    for (auto& member : instance.memberInputs) {
        member.present = false;
    }
    for (const auto& identity : std::span(party).first(count)) {
        MemberInputState* selected = nullptr;
        for (auto& member : instance.memberInputs) {
            if (member.life.member.identity == identity.identity
                && member.life.member.joinedRevision == identity.joinedRevision) {
                selected = &member;
                break;
            }
        }
        if (!selected) {
            for (auto& member : instance.memberInputs) {
                const bool retained =
                    std::ranges::any_of(std::span(party).first(count), [&](const auto& row) {
                        return row.identity == member.life.member.identity
                               && row.joinedRevision == member.life.member.joinedRevision;
                    });
                if (!member.present && !retained) {
                    selected = &member;
                    break;
                }
            }
        }
        if (!selected) {
            instance.partySnapshotValid = false;
            break;
        }
        bap::ActivityLinkView link{};
        const bool live =
            bap::mission_member_link_view(instance.view.binding, identity.identity, link);
        if (selected->life.bind(
                identity, live ? link.activityClientGeneration : 0, live ? link.playerKey : 0)) {
            selected->triggerOccupancy.fill({});
            selected->ghostObservations.fill({});
            selected->objectInteractionObservations.fill({});
        }
        selected->present = true;
    }
    for (auto& member : instance.memberInputs) {
        if (!member.present) {
            member = {};
        }
    }
}

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
        refresh_mission_members(instance);
        FireteamLife counts{};
        if (!instance.partySnapshotValid) {
            counts.add(PlayerLife::unknown);
        }
        for (const auto& member : instance.memberInputs) {
            if (member.present) {
                counts.add(member.life.life());
            }
        }
        if (instance.fireteamLifePublished && counts == instance.lastFireteamLife) {
            continue;
        }
        instance.lastFireteamLife = counts;
        instance.fireteamLifePublished = true;
        host::Event event = state_edge_event(instance);
        event.kind = host::EventKind::fireteamState;
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
    refresh_mission_members(instance);
    auto* member = member_input(instance, sense.sourceGeneration);
    if (!member) {
        return;
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
        PlayerLifeObservation& level =
            member->life.slots[observation.key.slotIndex - kFirstParticipationSlot];
        update_player_life(
            level, std::span(sense.values).subspan(observation.firstValue, observation.valueCount));
        // The host writes its own player key on every participation record of this link, so a
        // record that reports state is that player's.
        if ((level.seen & kLifeSeenKey) == 0 && (level.seen & kLifeSeenState) != 0
            && member->life.playerKey != 0) {
            level.playerKey = member->life.playerKey;
            level.seen |= kLifeSeenKey;
        }
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
        host::Event admitted = event;
        if (admitted.attemptGeneration == 0) {
            admitted.attemptGeneration = instance.dispatchAttemptGeneration != 0
                                             ? instance.dispatchAttemptGeneration
                                             : instance.attempt.generation;
        }
        instance.scriptEvents.push_back(admitted);
    } catch (const std::bad_alloc&) {
        fault_instance(instance, "mission event allocation failed");
    }
}

/**
 * Queues an exact host-policy transition without consuming a client mission-input sequence.
 * The policy may sit on any link that carries the program's output, so `admitted_provocation`
 * decides which links' provocations are the squad's own.
 * Lock order: the lane-0 receive path calls this under the peer lock, and `service` takes the
 * peer lock under this one through the actor-command policy. Both run only on the server
 * service thread, so the two orders cannot deadlock; moving either path to another thread would.
 * @param binding Activity generation that accepted the damage.
 * @param sourceGeneration ActivityClient generation that owns the policy.
 * @param registryKey Authored squad registry key.
 * @param slotIndex Native squad slot index.
 */
void report_squad_provoked(const state::activity::SessionBinding& binding,
                           std::uint64_t sourceGeneration,
                           std::uint32_t registryKey,
                           std::uint16_t slotIndex) noexcept {
    if (sourceGeneration == 0 || registryKey == 0
        || slotIndex > static_cast<std::uint16_t>((std::numeric_limits<std::int16_t>::max)())) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    auto* instance = find_instance(binding);
    if (instance != nullptr && admitted_provocation(*instance, sourceGeneration)) {
        host::Event event{};
        event.kind = host::EventKind::squadProvoked;
        event.binding = binding;
        event.sequence = instance->missionStateRevision;
        event.missionSequence = instance->lastMissionSequence;
        // Lua and `eligible_event` read shared simulation as the route's input.
        event.sourceGeneration = instance->view.activityClientGeneration;
        event.tick = GetTickCount64();
        event.firstRegistryKey = registryKey;
        event.firstSlotType = sdk::format::kSquadSlotType;
        event.firstSlotIndex = slotIndex;
        push_script_event(*instance, event);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/**
 * Queues one replicated damage level for a squad slot, once per change.
 * @param binding Activity generation that accepted the entity update.
 * @param sourceGeneration ActivityClient generation of the link that sent the entity update.
 * @param registryKey Authored registry key.
 * @param slotType Authored slot type: the squad's, or the combatant's own.
 * @param slotIndex Authored slot index.
 * @param health Pool 0 of the damage component, 0 to kDamageLevelFull.
 * @param shield Pool 1 of the damage component, 0 to kDamageLevelFull.
 */
void report_squad_damage(const state::activity::SessionBinding& binding,
                         std::uint64_t sourceGeneration,
                         std::uint32_t registryKey,
                         std::uint8_t slotType,
                         std::uint16_t slotIndex,
                         std::uint16_t health,
                         std::uint16_t shield) noexcept {
    constexpr std::uint16_t full = middleware::gameplay::external::kDamageLevelFull;
    if (sourceGeneration == 0 || registryKey == 0 || health > full || shield > full
        || slotIndex > static_cast<std::uint16_t>((std::numeric_limits<std::int16_t>::max)())) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    auto* instance = find_instance(binding);
    // A level that finds no mission, or comes from a link that does not simulate its target, is
    // dropped without a log line: every update of every link reaches this call.
    if (instance != nullptr) {
        push_entity_damage(*instance,
                           sourceGeneration,
                           registryKey,
                           slotType,
                           slotIndex,
                           static_cast<float>(health) / static_cast<float>(full),
                           static_cast<float>(shield) / static_cast<float>(full),
                           GetTickCount64());
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Retains the last VM stage and status shown on the panel. */
void note_vm_status(RuntimeInstance& instance,
                    std::string_view stage,
                    std::string_view status) noexcept {
    copy_text(instance.lastVmStage, stage);
    copy_text(instance.lastVmStatus, status);
}

/** Native Sense levels cannot cross an attempt or ActivityClient generation boundary. */
void clear_sense_levels(RuntimeInstance& instance) noexcept {
    instance.triggerOccupancy = {};
    instance.squadObservations = {};
    instance.squadShadows = {};
    instance.squadShadowAtCapacity = false;
    instance.ghostObservations = {};
    instance.damageObservations = {};
    instance.combatantDamageObservations = {};
    instance.entityDamageObservations = {};
    instance.deviceObservations = {};
    instance.objectInteractionObservations = {};
    instance.objectStateObservations = {};
    instance.actorPathObservations = {};
    instance.sceneObservations = {};
    instance.objectiveObservations = {};
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
    instance.pinCount = 0;
    std::vector<PinBubbleRow>{}.swap(instance.pinBubbles);
    std::vector<host::PendingScriptableOverride>{}.swap(instance.estate);
    instance.estateCopied = false;
    instance.liveLinkCount = 0;
    instance.entitySourceDamageObservations.fill({});
    instance.damageEntitySources.fill({});
    instance.nextEntityDamageTick = 0;
    instance.view = {};
    instance.identity = {};
    instance.programKey = {};
    instance.lastVmStage = {};
    instance.lastVmStatus = {};
    instance.attempt = {};
    instance.dispatchAttemptGeneration = 0;
    instance.dispatchInputSequence = 0;
    instance.eventsSeen = 0;
    instance.eventsCommitted = 0;
    instance.intentsTransportStaged = 0;
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
    instance.pendingTimerEvent = {};
    instance.intentAttempts = 0;
    instance.startAttempts = 0;
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
    reset_shared_observations(instance);
    instance.sessionRoster = {};
    instance.sessionRosterObserved = false;
    instance.memberInputs.clear();
    instance.partySnapshotValid = false;
    instance.lastFireteamLife = {};
    instance.fireteamLifePublished = false;
    std::vector<host::Event>{}.swap(instance.scriptEvents);
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
    if (instance.attempt.generation != snapshot.state.attempt.generation) {
        clear_sense_levels(instance);
        reset_member_observations(instance);
        // Player life is kept: the client sends only changed fields and never resends the region.
        instance.fireteamLifePublished = false;
        instance.timerPending = false;
        instance.pendingTimerEvent = {};
    }
    lua_vm::publish_attempt(instance.vm, snapshot.state.attempt);
    if (!lua_vm::publish_device_requests(instance.vm, snapshot.state.deviceRequests)) {
        lua_vm::fault(instance.vm, "device request snapshot allocation failed");
        instance.programStatus = ProgramStatus::programError;
    }
    publish_ghost_levels(instance);
    if (!lua_vm::publish_populations(instance.vm, snapshot.state.squadPopulations)) {
        lua_vm::fault(instance.vm, "population snapshot allocation failed");
        instance.programStatus = ProgramStatus::programError;
    }
    accept_compare_fields(instance, snapshot);
}

/**
 * The links a target's pin still counts on: the route, which stands even for a public target, and
 * every joined private link. They change only when a link joins, leaves or rejoins, so one read
 * per service pass serves every admission and device read in it.
 */
bool read_live_links(RuntimeInstance& instance) noexcept {
    std::array<bap::ActivityLinkView, state::activity::entity_slots::kMemberLeaseRowCount> links{};
    const std::size_t linkCount = bap::mission_route_candidates(
        instance.view.binding, {}, instance.view.activityClientGeneration, links);
    std::array<std::uint64_t, kLiveLinkCapacity> live{};
    std::size_t count = 0;
    live[count++] = instance.view.activityClientGeneration;
    for (const bap::ActivityLinkView& link : std::span(links).first(linkCount)) {
        if (link.activityClientGeneration != instance.view.activityClientGeneration) {
            live[count++] = link.activityClientGeneration;
        }
    }
    const auto next = std::span(live).first(count);
    const bool changed = !std::ranges::equal(next, live_links(instance));
    instance.liveLinks = live;
    instance.liveLinkCount = count;
    return changed;
}

/**
 * A device request is pinned to the link that carried it, and only that link's report applies it.
 * The script reads it as applied while that link is still live.
 */
bool publish_device_sources(const RuntimeInstance& instance, lua_vm::Vm& vm) noexcept {
    return lua_vm::publish_device_sources(vm, live_links(instance));
}

/** Copies the committed attempt, revision, cursors, phase and intent head into the instance. */
void accept_compare_fields(RuntimeInstance& instance,
                           const mission_state::Snapshot& snapshot) noexcept {
    instance.attempt = snapshot.state.attempt;
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
    if (!lua_vm::snapshot_intents(instance.vm, pendingIntents)) {
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
 * them, and the unbounded Host feed retains an unsettled row until it is settled.
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
            host::Event& head = instance.scriptEvents[instance.scriptEventRead];
            head.tick = now;
            static_cast<void>(dispatch_event(instance, head, nullptr, nullptr, now));
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
            instance.pendingTimerEvent.attemptGeneration = instance.attempt.generation;
            instance.pendingTimerEvent.binding = instance.view.binding;
            instance.pendingTimerEvent.timerName = selected->key;
            instance.pendingTimerEvent.sequence = selected->sequence;
            instance.pendingTimerEvent.tick = now;
            instance.pendingTimerEvent.sourceGeneration = instance.view.activityClientGeneration;
            instance.pendingTimerEvent.timerDeadlineTick = selected->deadlineTick;
            instance.pendingTimerEvent.timerSequence = selected->sequence;
            instance.pendingTimerEvent.missionSequence = instance.lastMissionSequence;
            instance.pendingTimerEvent.kind = host::EventKind::timerElapsed;
            instance.timerPending = true;
        }
        static_cast<void>(
            dispatch_event(instance, instance.pendingTimerEvent, nullptr, nullptr, now));
        instance.pendingTimerEvent = {};
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

namespace {

// A step this long blocks the game's loopback sends; the step that took it is named.
constexpr std::uint64_t kSlowStepMilliseconds = 40;

/** Runs one step of the mission tick and logs it when it runs past the slow limit. */
template <typename Step> void timed_step(const char* name, Step&& step) noexcept {
    const std::uint64_t started = GetTickCount64();
    step();
    const std::uint64_t elapsed = GetTickCount64() - started;
    if (elapsed < kSlowStepMilliseconds) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=core stage=service result=slow step=mission.%s ms=%llu",
                                      name,
                                      static_cast<unsigned long long>(elapsed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/**
 * Runs one mission tick under the runtime lock, timing each step.
 * @param now Service tick every step measures its deadlines from.
 */
void service(std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_pathReady) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    timed_step("synchronize", [now] { synchronize_instances(now); });
    timed_step("links", [] {
        for (RuntimeInstance& instance : g_instances) {
            if (instance.occupied && read_live_links(instance)
                && instance.programStatus == ProgramStatus::loaded
                && !publish_device_sources(instance, instance.vm)) {
                fault_instance(instance, "device request source allocation failed");
            }
        }
    });
    timed_step("starts", [now] { service_pending_starts(now); });
    timed_step("delivery_events", [now] { consume_delivery_events(now); });
    timed_step("inputs", [now] { consume_mission_inputs(now); });
    timed_step("scriptless", [] { retire_scriptless_inputs(); });
    timed_step("timers", [now] { service_timers(now); });
    timed_step("entity_damage", [now] {
        for (auto& instance : g_instances) {
            if (instance.occupied && instance.programStatus == ProgramStatus::loaded) {
                push_entity_damage_edges(instance, now);
            }
        }
    });
    timed_step("script_events", [now] { service_script_events(now); });
    timed_step("dispatch", [now] {
        for (RuntimeInstance& instance : g_instances) {
            if (instance.occupied) {
                if (instance.programStatus == ProgramStatus::programError) {
                    reconcile_terminal_delivery(instance);
                } else {
                    dispatch_intent(instance, now);
                }
            }
        }
    });
    timed_step("reload", [now] { service_reload(now); });
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies every open instance row and every retained attach row for the panel. */
void snapshot(DiagnosticsSnapshot& output) noexcept {
    std::destroy_at(&output);
    std::construct_at(&output);
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

/** @return True when mission scripting is on and its paths resolved. */
bool runtime_ready() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool ready = g_enabled && g_pathReady;
    ReleaseSRWLockShared(&g_lock);
    return ready;
}

/** Closes every instance and clears every global this unit owns. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (RuntimeInstance& instance : g_instances) {
        clear_instance(instance);
    }
    clear_all_pending_events();
    clear_attach_diagnostics();
    clear_script_sources();
    clear_feed_cursors();
    clear_reload_mailbox();
    g_enabled = false;
    g_pathReady = false;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::activity::mission
