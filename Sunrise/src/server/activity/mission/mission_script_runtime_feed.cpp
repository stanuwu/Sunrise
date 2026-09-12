/**
 * The two Host feeds, the queue of accepted rows, and the VM callback each row reaches.
 * Every function here runs under the mission runtime lock its caller already holds.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../host_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

/** One queued mission event and the retry bookkeeping the drain uses. */
struct PendingMissionEvent final {
    host::Event event{};
    host::SenseObservationSnapshot sense{};
    host::ClientMessageSnapshot clientMessage{};
    std::uint64_t firstAttempt{};
    std::uint64_t nextAttempt{};
    std::uint32_t attempts{};
    bool missionSequenceObserved{};
    bool senseAvailable{};
    bool clientMessageAvailable{};
    bool callbackEligible{};
    bool eligibilityResolved{};
    bool occupied{};
};

/** The queue grows with real accepted input and is drained in the tick it arrives. */
std::vector<PendingMissionEvent> g_pendingMissionEvents{};
host::EventCursor g_eventCursor{};
host::MissionInputCursor g_missionInputCursor{};

void clear_pending_event(PendingMissionEvent& pending) noexcept {
    SecureZeroMemory(&pending, sizeof(pending));
}

/** @return True for the host events that report an output's progress, not an input. */
[[nodiscard]] bool delivery_lifecycle_event(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::authStateCommitted:
    case host::EventKind::authStateTransportStaged:
    case host::EventKind::authStateCanceled:
    case host::EventKind::incidentQueued:
    case host::EventKind::incidentTransportStaged:
    case host::EventKind::incidentCanceled:
    case host::EventKind::incidentRefused:
    case host::EventKind::scriptableOverrideCommitted:
    case host::EventKind::scriptableOverrideTransportStaged:
    case host::EventKind::scriptableOverrideCanceled:
    case host::EventKind::operatorRefused:
        return true;
    default:
        return false;
    }
}

/**
 * @return True for a row that arrives on the ordered mission-input feed and owns a sequence.
 * A host-state row must never answer true. It would consume a mission-input sequence it does not
 * own, which faults the binding on the next real input.
 */
[[nodiscard]] bool host_feed_row(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::timerElapsed:
    case host::EventKind::effectResult:
    case host::EventKind::phaseEntered:
    case host::EventKind::triggerEntered:
    case host::EventKind::triggerExited:
    case host::EventKind::squadState:
    case host::EventKind::squadProvoked:
    case host::EventKind::entitySpawned:
    case host::EventKind::entityDied:
    case host::EventKind::sceneFinished:
    case host::EventKind::objectiveProgress:
    case host::EventKind::sessionJoined:
    case host::EventKind::sessionLeft:
    case host::EventKind::playerTrigger:
    case host::EventKind::cinematicStarted:
    case host::EventKind::cinematicSkipRequested:
    case host::EventKind::cinematicTerminated:
    case host::EventKind::actorPathState:
    case host::EventKind::damageState:
    case host::EventKind::deviceState:
    case host::EventKind::objectState:
    case host::EventKind::fireteamState:
    case host::EventKind::objectInteracted:
    case host::EventKind::ghostLinkState:
        return false;
    default:
        return true;
    }
}

/** True when the event may reach a callback for this instance's ActivityClient generation. */
[[nodiscard]] bool eligible_event(const RuntimeInstance& instance,
                                  const host::Event& event) noexcept {
    if (event.kind == host::EventKind::timerElapsed) {
        return true;
    }
    if (event.sourceGeneration != instance.view.activityClientGeneration) {
        return false;
    }
    return event.kind == host::EventKind::clientStateChanged
           || event.kind == host::EventKind::incidentReceived
           || event.kind == host::EventKind::clientMessageReceived
           || event.kind == host::EventKind::effectResult
           || event.kind == host::EventKind::phaseEntered
           || event.kind == host::EventKind::triggerEntered
           || event.kind == host::EventKind::triggerExited
           || event.kind == host::EventKind::squadState
           || event.kind == host::EventKind::squadProvoked
           || event.kind == host::EventKind::entitySpawned
           || event.kind == host::EventKind::entityDied
           || event.kind == host::EventKind::sceneFinished
           || event.kind == host::EventKind::objectiveProgress
           || event.kind == host::EventKind::entitySlotsRequested
           || event.kind == host::EventKind::sessionJoined
           || event.kind == host::EventKind::sessionLeft
           || event.kind == host::EventKind::playerTrigger
           || event.kind == host::EventKind::cinematicStarted
           || event.kind == host::EventKind::actorPathState
           || event.kind == host::EventKind::damageState
           || event.kind == host::EventKind::deviceState
           || event.kind == host::EventKind::objectState
           || event.kind == host::EventKind::fireteamState
           || event.kind == host::EventKind::objectInteracted
           || event.kind == host::EventKind::ghostLinkState
           || event.kind == host::EventKind::cinematicSkipRequested
           || event.kind == host::EventKind::cinematicTerminated
           || delivery_lifecycle_event(event.kind) || event.has_sense_observations();
}

/** Faults the instance unless the ordered mission input arrives with no gap, starting at one. */
[[nodiscard]] bool validate_mission_sequence(RuntimeInstance& instance,
                                             const host::Event& event) noexcept {
    if (event.kind != host::EventKind::senseUpdate
        && event.kind != host::EventKind::incidentReceived
        && event.kind != host::EventKind::clientStateChanged
        && event.kind != host::EventKind::entitySlotsRequested
        && event.kind != host::EventKind::clientMessageReceived) {
        return true;
    }
    if (instance.lastMissionSequence == 0) {
        if (event.missionSequence == 1) {
            return true;
        }
        fault_instance(instance, "activity mission input did not start at sequence one");
        log_line(core::log::Level::warn, &instance, "events", "initial_binding_gap");
        return false;
    }
    const std::uint64_t expected =
        instance.lastMissionSequence == (std::numeric_limits<std::uint64_t>::max)()
            ? 1
            : instance.lastMissionSequence + 1;
    if (event.missionSequence == expected) {
        return true;
    }
    fault_instance(instance, "activity mission input sequence has a gap");
    log_line(core::log::Level::warn, &instance, "events", "binding_gap");
    return false;
}

/** One free queue row, growing the queue by one when none is free; null when it cannot grow. */
[[nodiscard]] PendingMissionEvent* free_pending_event() noexcept {
    for (PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (!pending.occupied) {
            return &pending;
        }
    }
    if (g_pendingMissionEvents.size() == g_pendingMissionEvents.max_size()) {
        return nullptr;
    }
    try {
        g_pendingMissionEvents.emplace_back();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    return &g_pendingMissionEvents.back();
}

/** Copies one accepted input and its values into a queue row; faults the instance when full. */
[[nodiscard]] bool queue_mission_event(RuntimeInstance* instance,
                                       const host::MissionInputEvent& input) noexcept {
    PendingMissionEvent* const pending = free_pending_event();
    if (pending == nullptr) {
        if (instance != nullptr) {
            fault_instance(*instance, "mission event queue allocation failed");
            clear_pending_events(instance->view.binding);
        }
        log_line(core::log::Level::warn, instance, "events", "allocation_failed");
        return false;
    }
    clear_pending_event(*pending);
    if (input.event.has_sense_observations()) {
        pending->senseAvailable =
            host::mission_input_sense_snapshot(input.sequence, pending->sense);
    }
    if (input.event.kind == host::EventKind::clientMessageReceived) {
        pending->clientMessageAvailable =
            host::mission_input_client_message_snapshot(input.sequence, pending->clientMessage);
    }
    pending->nextAttempt = 0;
    pending->event = input.event;
    if (instance != nullptr) {
        pending->callbackEligible = eligible_event(*instance, input.event);
        pending->eligibilityResolved = true;
    }
    pending->occupied = true;
    return true;
}

/** @return True when the left sequence comes before the right one across the wrap. */
[[nodiscard]] constexpr bool mission_sequence_precedes(std::uint64_t left,
                                                       std::uint64_t right) noexcept {
    // Sequences wrap, so ordering holds only inside half the 64-bit range.
    constexpr std::uint64_t halfRange = std::uint64_t{1} << 63U;
    return left != right && right - left < halfRange;
}

static_assert(mission_sequence_precedes((std::numeric_limits<std::uint64_t>::max)(), 1));
static_assert(!mission_sequence_precedes(1, (std::numeric_limits<std::uint64_t>::max)()));

/** @return True when the durable cursor already committed this retained input row. */
[[nodiscard]] bool mission_sequence_committed(std::uint64_t sequence,
                                              std::uint64_t committed) noexcept {
    return committed != 0
           && (sequence == committed || mission_sequence_precedes(sequence, committed));
}

/** True when the same binding still holds a queued row with an earlier mission sequence. */
[[nodiscard]] bool has_earlier_pending_event(const PendingMissionEvent& selected) noexcept {
    for (const PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (pending.occupied && &pending != &selected
            && same_binding(pending.event.binding, selected.event.binding)
            && mission_sequence_precedes(pending.event.missionSequence,
                                         selected.event.missionSequence)) {
            return true;
        }
    }
    return false;
}

/** TODO: no caller. Decide whether the drain gate retires on this or on `binding_matches`. */
[[nodiscard]] bool host_binding_active(const state::activity::SessionBinding& binding) noexcept {
    host::InstanceSnapshot snapshot{};
    return host::instance_snapshot(binding, snapshot) && snapshot.active;
}

/** Dispatches queued rows in mission-sequence order and retires those no callback can take. */
void drain_pending_mission_events(std::uint64_t now) noexcept {
    bool progressed = false;
    do {
        progressed = false;
        for (PendingMissionEvent& pending : g_pendingMissionEvents) {
            if (!pending.occupied || now < pending.nextAttempt
                || has_earlier_pending_event(pending)) {
                continue;
            }
            RuntimeInstance* const instance = find_instance(pending.event.binding);
            if (instance == nullptr) {
                if (!state::activity::binding_matches(pending.event.binding)) {
                    clear_pending_event(pending);
                    progressed = true;
                }
                continue;
            }
            if (instance->programStatus != ProgramStatus::loaded) {
                clear_pending_event(pending);
                progressed = true;
                continue;
            }
            if (instance->startPending) {
                continue;
            }
            if (instance->timerPending) {
                continue;
            }
            if (mission_sequence_committed(pending.event.missionSequence,
                                           instance->lastMissionSequence)) {
                clear_pending_event(pending);
                progressed = true;
                continue;
            }
            if (!pending.missionSequenceObserved) {
                if (!validate_mission_sequence(*instance, pending.event)) {
                    clear_pending_events(instance->view.binding);
                    progressed = true;
                    break;
                }
                pending.missionSequenceObserved = true;
            }
            if (!pending.eligibilityResolved) {
                pending.callbackEligible = eligible_event(*instance, pending.event);
                pending.eligibilityResolved = true;
            }
            if (!pending.callbackEligible) {
                if (!commit_mission_state(
                        *instance, instance->missionStarted, pending.event.missionSequence)) {
                    clear_pending_events(instance->view.binding);
                    progressed = true;
                    break;
                }
                clear_pending_event(pending);
                progressed = true;
                continue;
            }
            if (pending.event.kind == host::EventKind::senseUpdate && !pending.senseAvailable) {
                fault_instance(*instance, "accepted mission Sense values were unavailable");
                clear_pending_events(instance->view.binding);
                log_line(core::log::Level::warn, instance, "events", "sense_unavailable");
                progressed = true;
                break;
            }
            if (pending.event.kind == host::EventKind::clientMessageReceived
                && !pending.clientMessageAvailable) {
                fault_instance(*instance,
                               "accepted mission client-message values were unavailable");
                clear_pending_events(instance->view.binding);
                log_line(core::log::Level::warn, instance, "events", "client_message_unavailable");
                progressed = true;
                break;
            }
            lua_vm::Intent intent{};
            if (instance->deliveryStage != DeliveryStage::idle
                || lua_vm::pending_intent(instance->vm, intent)) {
                continue;
            }
            const bool firstAttempt = pending.attempts == 0;
            if (firstAttempt) {
                pending.firstAttempt = now;
            }
            ++pending.attempts;
            const host::SenseObservationSnapshot* const sense =
                pending.event.kind == host::EventKind::senseUpdate && pending.senseAvailable
                    ? &pending.sense
                    : nullptr;
            const host::ClientMessageSnapshot* const clientMessage =
                pending.event.kind == host::EventKind::clientMessageReceived
                        && pending.clientMessageAvailable
                    ? &pending.clientMessage
                    : nullptr;
            static_cast<void>(
                dispatch_event(*instance, pending.event, sense, clientMessage, firstAttempt, now));
            clear_pending_event(pending);
            progressed = true;
        }
    } while (progressed);
}

/** @return True when one exact accepted sequence is already retained locally or in this read. */
[[nodiscard]] bool input_sequence_retained(const state::activity::SessionBinding& binding,
                                           std::uint64_t sequence,
                                           const host::MissionInputRead& inputs) noexcept {
    for (const PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (pending.occupied && same_binding(pending.event.binding, binding)
            && pending.event.missionSequence == sequence) {
            return true;
        }
    }
    for (std::size_t index = 0; index < inputs.count; ++index) {
        if (same_binding(inputs.events[index].event.binding, binding)
            && inputs.events[index].event.missionSequence == sequence) {
            return true;
        }
    }
    return false;
}

/** @return True when every accepted but uncommitted sequence is present in this read or local
 * queue. */
[[nodiscard]] bool
outstanding_input_interval_complete(const state::activity::SessionBinding& binding,
                                    const mission_state::InputSequenceSnapshot& state,
                                    const host::MissionInputRead& inputs) noexcept {
    if (state.issued < state.committed) {
        return false;
    }
    const std::uint64_t outstanding = state.issued - state.committed;
    if (outstanding > g_pendingMissionEvents.size() + inputs.count) {
        return false;
    }
    std::uint64_t sequence = state.committed;
    for (std::uint64_t index = 0; index < outstanding; ++index) {
        ++sequence;
        if (!input_sequence_retained(binding, sequence, inputs)) {
            return false;
        }
    }
    return true;
}

/** Faults only retained bindings whose durable uncommitted interval is provably incomplete. */
void reconcile_input_feed_loss(const host::MissionInputRead& inputs) noexcept {
    host::DiagnosticsSnapshot hostState{};
    host::snapshot(hostState);
    for (std::size_t index = 0; index < hostState.instanceCount; ++index) {
        const host::InstanceSnapshot& hostInstance = hostState.instances[index];
        if (!state::activity::binding_matches(hostInstance.binding)) {
            continue;
        }
        mission_state::InputSequenceSnapshot inputState{};
        if (!mission_state::input_sequence_snapshot(hostInstance.binding, inputState)
            || inputState.faulted
            || outstanding_input_interval_complete(hostInstance.binding, inputState, inputs)) {
            continue;
        }
        mission_state::Snapshot snapshot{};
        const mission_state::Status status =
            mission_state::fault_input_feed(hostInstance.binding, snapshot);
        RuntimeInstance* const instance = find_instance(hostInstance.binding);
        if (status != mission_state::Status::ready) {
            log_line(core::log::Level::warn,
                     instance,
                     "events",
                     mission_state::status_name(status),
                     "reason=feed_gap_fault_refused");
            continue;
        }
        if (instance != nullptr) {
            accept_mission_state(*instance, snapshot);
            lua_vm::fault(instance->vm, "accepted mission input feed lost a row");
            instance->programStatus = ProgramStatus::programError;
        }
        log_line(core::log::Level::warn, instance, "events", "feed_gap_faulted");
        clear_pending_events(hostInstance.binding);
    }
}

/** Reads one page of the ordered feed and queues every row not yet committed. */
[[nodiscard]] bool consume_mission_input_page() noexcept {
    host::MissionInputRead inputs{};
    host::read_mission_inputs_after(g_missionInputCursor, inputs);
    if (inputs.reset) {
        g_missionInputCursor = {inputs.cursor.generation, 0};
        reconcile_input_feed_loss(inputs);
        log_line(core::log::Level::warn, nullptr, "events", "mission_feed_reset");
    }
    if (inputs.gap) {
        log_line(core::log::Level::warn, nullptr, "events", "mission_feed_gap");
        reconcile_input_feed_loss(inputs);
    }
    for (std::size_t index = 0; index < inputs.count; ++index) {
        const host::MissionInputEvent& input = inputs.events[index];
        RuntimeInstance* const instance = find_instance(input.event.binding);
        mission_state::InputSequenceSnapshot inputState{};
        const bool hasInputState =
            mission_state::input_sequence_snapshot(input.event.binding, inputState);
        if ((instance == nullptr && !state::activity::binding_matches(input.event.binding))
            || (instance != nullptr && instance->programStatus != ProgramStatus::loaded)
            || (hasInputState
                && (inputState.faulted
                    || mission_sequence_committed(input.event.missionSequence,
                                                  inputState.committed)))) {
            g_missionInputCursor.generation = inputs.cursor.generation;
            g_missionInputCursor.sequence = input.sequence;
            continue;
        }
        if (instance != nullptr
            && mission_sequence_committed(input.event.missionSequence,
                                          instance->lastMissionSequence)) {
            g_missionInputCursor.generation = inputs.cursor.generation;
            g_missionInputCursor.sequence = input.sequence;
            continue;
        }
        if (instance != nullptr) {
            lua_vm::Snapshot diagnostics{};
            lua_vm::snapshot(instance->vm, diagnostics);
            if (diagnostics.faulted) {
                g_missionInputCursor.generation = inputs.cursor.generation;
                g_missionInputCursor.sequence = input.sequence;
                continue;
            }
        }
        if (!queue_mission_event(instance, input)) {
            return false;
        }
        g_missionInputCursor.generation = inputs.cursor.generation;
        g_missionInputCursor.sequence = input.sequence;
    }
    return inputs.count == host::kMissionInputReadPageSize;
}

} // namespace

/** Retires every queued mission event that belongs to one binding. */
void clear_pending_events(const state::activity::SessionBinding& binding) noexcept {
    for (PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (pending.occupied && same_binding(pending.event.binding, binding)) {
            clear_pending_event(pending);
        }
    }
}

/** Retires every queued mission event that belongs to one binding. */
void clear_all_pending_events() noexcept {
    std::vector<PendingMissionEvent>{}.swap(g_pendingMissionEvents);
}

/** Keeps accepted values but removes every VM- and ActivityClient-generation-local decision. */
void reset_pending_events_for_reattach(const state::activity::SessionBinding& binding) noexcept {
    for (PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (!pending.occupied || !same_binding(pending.event.binding, binding)) {
            continue;
        }
        pending.firstAttempt = 0;
        pending.nextAttempt = 0;
        pending.attempts = 0;
        pending.missionSequenceObserved = false;
        pending.callbackEligible = false;
        pending.eligibilityResolved = false;
    }
}

/** Retires rows as soon as authoritative State no longer owns their exact binding. */
void retire_unbound_pending_events() noexcept {
    for (PendingMissionEvent& pending : g_pendingMissionEvents) {
        if (pending.occupied && !state::activity::binding_matches(pending.event.binding)) {
            clear_pending_event(pending);
        }
    }
}

/** @return Queued rows still owed to one exact binding. */
std::size_t pending_event_count(const state::activity::SessionBinding& binding) noexcept {
    std::size_t count = 0;
    for (const PendingMissionEvent& pending : g_pendingMissionEvents) {
        count += pending.occupied && same_binding(pending.event.binding, binding) ? 1U : 0U;
    }
    return count;
}

/** @return True when one queued accepted row is still owed to this instance. */
bool has_pending_host_input(const RuntimeInstance& instance) noexcept {
    return std::any_of(g_pendingMissionEvents.begin(),
                       g_pendingMissionEvents.end(),
                       [&instance](const PendingMissionEvent& pending) noexcept {
                           return pending.occupied
                                  && same_binding(pending.event.binding, instance.view.binding);
                       });
}

/** Points both Host cursors at the current feed heads and replays retained accepted inputs. */
void reset_feed_cursors() noexcept {
    g_eventCursor = host::current_event_cursor();
    g_missionInputCursor = host::current_mission_input_cursor();
    // Durable per-binding cursors suppress callbacks that already committed; an evicted
    // predecessor still faults as a gap.
    g_missionInputCursor.sequence = 0;
}

/** Clears both Host cursors. */
void clear_feed_cursors() noexcept {
    g_eventCursor = {};
    g_missionInputCursor = {};
}

/**
 * Runs one event through the VM, commits what it changed, and faults on a script failure.
 * @param sense Values owned by a Sense row, or null.
 * @param clientMessage Envelope snapshot owned by a client-message row, or null.
 * @param firstAttempt True on the first delivery attempt, which is the one that counts it.
 * @return The VM call status; `inactive` when no callback could take the event.
 */
lua_vm::CallStatus dispatch_event(RuntimeInstance& instance,
                                  const host::Event& event,
                                  const host::SenseObservationSnapshot* sense,
                                  const host::ClientMessageSnapshot* clientMessage,
                                  bool firstAttempt,
                                  std::uint64_t now) noexcept {
    if (instance.programStatus == ProgramStatus::missing
        && event.kind == host::EventKind::senseUpdate && sense != nullptr) {
        push_squad_edges(instance, *sense);
        return lua_vm::CallStatus::inactive;
    }
    if (instance.programStatus != ProgramStatus::loaded || !eligible_event(instance, event)) {
        return lua_vm::CallStatus::inactive;
    }
    if (firstAttempt) {
        ++instance.eventsSeen;
        instance.lastEventSequence = event.sequence;
    }
    // The three region numbers the script is about to read. A report restates only the leg it
    // moved, so `pending` and `current` read -1 on most reports and `held` is the one that says
    // where the client is standing.
    if (firstAttempt && event.kind == host::EventKind::clientStateChanged) {
        std::array<char, 64> legs{};
        const int written =
            std::snprintf(legs.data(),
                          legs.size(),
                          "held=%d pending=%d current=%d",
                          event.heldRegionIndex,
                          event.clientStateHasRegion ? event.regionIndex : -1,
                          event.clientStateHasCurrentRegion ? event.currentRegionIndex : -1);
        if (written > 0) {
            log_line(core::log::Level::debug,
                     &instance,
                     "client_state",
                     "legs",
                     {legs.data(), static_cast<std::size_t>(written)});
        }
    }
    const lua_vm::CallStatus status = lua_vm::dispatch(instance.vm, event, clientMessage, now);
    if (event.kind == host::EventKind::clientStateChanged) {
        // A pending-region report can name the next slice while the player still holds the old
        // one, so the held region wins.
        if (event.heldRegionIndex >= 0) {
            instance.activeRegion = event.heldRegionIndex;
        } else if (event.currentRegionIndex >= 0) {
            instance.activeRegion = event.currentRegionIndex;
        }
    }
    if (firstAttempt && event.kind == host::EventKind::incidentReceived) {
        push_player_trigger(instance, event);
        push_cinematic(instance, event);
    }
    if (event.kind == host::EventKind::senseUpdate && sense != nullptr) {
        if (firstAttempt) {
            observe_player_life(instance, *sense);
            publish_fireteam_life(now);
        }
        push_trigger_edges(instance, *sense);
        push_ghost_edges(instance, *sense);
        push_object_interaction_edges(instance, *sense);
        push_damage_edges(instance, *sense);
        push_actor_path_edges(instance, *sense);
        push_squad_edges(instance, *sense);
        push_combatant_damage_edges(instance, *sense);
        push_device_edges(instance, *sense);
        push_scene_edges(instance, *sense);
        push_objective_edges(instance, *sense);
    }
    note_vm_status(instance, "event", lua_vm::status_name(status));
    if (firstAttempt && instance.eventsSeen == 1) {
        log_line(core::log::Level::info,
                 &instance,
                 "dispatch",
                 lua_vm::status_name(status),
                 event.kind == host::EventKind::senseUpdate            ? "sense"
                 : event.kind == host::EventKind::clientStateChanged   ? "client_state"
                 : event.kind == host::EventKind::incidentReceived     ? "incident"
                 : event.kind == host::EventKind::entitySlotsRequested ? "entity_slots_requested"
                 : event.kind == host::EventKind::timerElapsed         ? "timer"
                 : event.kind == host::EventKind::effectResult         ? "effect_result"
                                                                       : "client_message");
    }
    const std::uint64_t nextInputSequence =
        host_feed_row(event.kind) ? event.missionSequence : instance.lastMissionSequence;
    if (status == lua_vm::CallStatus::committed) {
        if (!commit_mission_state(instance, true, nextInputSequence)) {
            return lua_vm::CallStatus::scriptError;
        }
        ++instance.eventsCommitted;
        lua_vm::Snapshot diagnostics{};
        lua_vm::snapshot(instance.vm, diagnostics);
        if (event.kind == host::EventKind::clientStateChanged
            || diagnostics.stateRevision != instance.lastLoggedRevision) {
            instance.lastLoggedRevision = diagnostics.stateRevision;
            log_line(core::log::Level::debug,
                     &instance,
                     "event",
                     "committed",
                     event.kind == host::EventKind::senseUpdate          ? "sense"
                     : event.kind == host::EventKind::clientStateChanged ? "client_state"
                     : event.kind == host::EventKind::incidentReceived   ? "incident"
                     : event.kind == host::EventKind::entitySlotsRequested
                         ? "entity_slots_requested"
                     : event.kind == host::EventKind::timerElapsed ? "timer"
                     : event.kind == host::EventKind::effectResult ? "effect_result"
                                                                   : "client_message");
        }
        return status;
    }
    if (status == lua_vm::CallStatus::noHandler) {
        return commit_mission_state(instance, true, nextInputSequence)
                   ? status
                   : lua_vm::CallStatus::scriptError;
    }
    if (status == lua_vm::CallStatus::inactive) {
        return status;
    }
    lua_vm::Snapshot diagnostics{};
    lua_vm::snapshot(instance.vm, diagnostics);
    log_line(core::log::Level::warn,
             &instance,
             "event",
             lua_vm::status_name(status),
             {},
             diagnostics.lastError.data());
    persist_mission_fault(instance);
    return status;
}

/** Reads new host events, advances delivery, and queues only the lifecycle rows for scripts. */
void consume_delivery_events(std::uint64_t now) noexcept {
    host::EventRead events{};
    host::read_events_after(g_eventCursor, events);
    g_eventCursor = events.cursor;
    if (events.reset) {
        log_line(core::log::Level::warn, nullptr, "delivery", "event_feed_reset");
        return;
    }
    if (events.gap) {
        log_line(core::log::Level::warn, nullptr, "delivery", "event_feed_gap");
    }
    for (std::size_t index = 0; index < events.count; ++index) {
        RuntimeInstance* const instance = find_instance(events.events[index].binding);
        if (instance != nullptr) {
            observe_delivery_event(*instance, events.events[index], now);
            // Sense, client-state, incident and client-message rows reach the script through the
            // ordered mission-input feed. Only the delivery lifecycle rows belong in this queue.
            if (delivery_lifecycle_event(events.events[index].kind)) {
                push_script_event(*instance, events.events[index]);
            }
        }
    }
}

/**
 * Reads the ordered mission-input feed and drains the queue. The feed is unbounded and one read
 * copies at most a page, so a burst is consumed in the tick it arrives instead of a page a tick.
 */
void consume_mission_inputs(std::uint64_t now) noexcept {
    drain_pending_mission_events(now);
    while (consume_mission_input_page()) {
        drain_pending_mission_events(now);
    }
    drain_pending_mission_events(now);
}

} // namespace sunrise::server::activity::mission
