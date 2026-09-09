/**
 * The Activity Host instance table, event ring, reducer queue and Auth-state lane.
 * Helpers here need the Host runtime lock; the entry points take it themselves.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <new>
#include <vector>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "../../state/activity/runtime.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace detail {

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Instance, kInstanceCapacity> g_instances{};
std::vector<PendingInput> g_pending{};
std::array<Event, kEventCapacity> g_events{};
std::size_t g_pendingRead{};
std::size_t g_queuedIngress{};
std::size_t g_queuedControls{};
std::size_t g_eventStart{};
std::size_t g_eventCount{};
std::uint64_t g_sequence{};
std::uint64_t g_scriptableReservationGeneration{1};
std::uint64_t g_scriptableReservationSequence{};
std::uint64_t g_eventGeneration{1};
std::uint64_t g_touch{};
std::uint64_t g_droppedIngress{};
std::uint64_t g_refusedControls{};
std::uint64_t g_overwrittenEvents{};

/** @return True when the value stays inside the client's jump table. */
[[nodiscard]] bool lifetime_allowed(std::uint8_t value) noexcept {
    return value <= kMaximumLifetimeState;
}

/** Advances a diagnostic counter without making zero look like no event. */
[[nodiscard]] std::uint64_t next_nonzero(std::uint64_t value) noexcept {
    return value == (std::numeric_limits<std::uint64_t>::max)() ? 1 : value + 1;
}

/** Finds one exact instance while the runtime lock is held. */
[[nodiscard]] Instance* find_instance(const state::activity::SessionBinding& binding) noexcept {
    for (Instance& instance : g_instances) {
        if (instance.occupied && same_binding(instance.view.binding, binding)) {
            return &instance;
        }
    }
    return nullptr;
}

/** @return True when this exact binding already has an operator request waiting to reduce. */
[[nodiscard]] bool has_queued_control(const state::activity::SessionBinding& binding) noexcept {
    for (std::size_t index = g_pendingRead; index < g_pending.size(); ++index) {
        const PendingInput& pending = g_pending[index];
        if (pending.kind == PendingKind::authControl
            && same_binding(pending.control.binding, binding)) {
            return true;
        }
        if (pending.kind == PendingKind::incidentControl
            && same_binding(pending.incidentControl.binding, binding)) {
            return true;
        }
        if (pending.kind == PendingKind::scriptableControl
            && same_binding(pending.scriptableControl.binding, binding)) {
            return true;
        }
    }
    return false;
}

/** A refused input is the one loss the host cannot see later, so it says so when it happens. */
void report_ingress_drop(PendingKind kind, const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=ingress result=drop reason=%s kind=%u queued=%zu",
                      reason,
                      static_cast<unsigned>(kind),
                      g_pending.size());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Appends one owned reducer row while the runtime lock is held. @return False when full. */
bool append_pending(const PendingInput& pending) noexcept {
    if (g_pending.size() == g_pending.max_size()) {
        report_ingress_drop(pending.kind, "queue_full");
        return false;
    }
    try {
        g_pending.push_back(pending);
    } catch (const std::bad_alloc&) {
        report_ingress_drop(pending.kind, "no_memory");
        return false;
    }
    return true;
}

/** Allocates one diagnostic instance without evicting a binding active in this service slice. */
[[nodiscard]] Instance* ensure_instance(const state::activity::SessionBinding& binding) noexcept {
    if (Instance* const current = find_instance(binding); current != nullptr) {
        return current;
    }
    Instance* selected = nullptr;
    for (Instance& instance : g_instances) {
        if (!instance.occupied) {
            selected = &instance;
            break;
        }
        if (!instance.view.active && !instance.view.outputPending
            && !instance.view.scriptableReservationPending && instance.view.incidentsPending == 0
            && (selected == nullptr || instance.lastTouched < selected->lastTouched)) {
            selected = &instance;
        }
    }
    if (selected == nullptr) {
        return nullptr;
    }
    clear_instance(*selected);
    selected->occupied = true;
    selected->view.binding = binding;
    selected->view.lifetimeState = kDefaultLifetimeState;
    return selected;
}

/** Moves one instance to the newest eviction position. */
void touch(Instance& instance) noexcept {
    g_touch = next_nonzero(g_touch);
    instance.lastTouched = g_touch;
}

/** Appends one event in oldest-to-newest ring order. */
void append_event(Event& event) noexcept {
    stamp_mission_sequence(event);
    g_sequence = next_nonzero(g_sequence);
    event.sequence = g_sequence;
    std::size_t index = (g_eventStart + g_eventCount) % g_events.size();
    if (g_eventCount == g_events.size()) {
        index = g_eventStart;
        g_eventStart = (g_eventStart + 1) % g_events.size();
        ++g_overwrittenEvents;
    } else {
        ++g_eventCount;
    }
    g_events[index] = event;
}

/** Cancels one committed output while the runtime lock is held. */
void cancel_output(Instance& instance, std::uint64_t now) noexcept {
    if (!instance.view.outputPending) {
        return;
    }
    Event event{};
    event.binding = instance.view.binding;
    event.tick = now;
    if (instance.view.outputKind == OutputKind::incident) {
        IncidentRecord* const record =
            find_incident(instance.view.binding, instance.view.incidentRevision);
        if (record != nullptr) {
            record->status = IncidentStatus::canceled;
            fill_incident_event(event, record->incident, record->revision);
        }
        event.kind = EventKind::incidentCanceled;
        instance.view.incidentsPending = 0;
    } else if (instance.view.outputKind == OutputKind::scriptableOverride) {
        event.kind = EventKind::scriptableOverrideCanceled;
        event.scriptableRevision = instance.view.scriptableRevision;
        instance.pendingScriptable = {};
    } else {
        event.stateRevision = instance.view.stateRevision;
        event.lifetimeState = instance.view.lifetimeState;
        event.kind = EventKind::authStateCanceled;
    }
    instance.view.outputPending = false;
    instance.view.outputKind = OutputKind::none;
    instance.view.outputStatus = OutputStatus::canceled;
    append_event(event);
    instance.view.lastEventSequence = g_sequence;
}

/** Retains one safe committed client State change in Host and ordered mission histories. */
void apply_client_state_change(const ClientStateChangeInput& input, std::uint64_t now) noexcept {
    Instance* const instance = find_instance(input.binding);
    if (instance == nullptr || !instance->view.active) {
        ++g_droppedIngress;
        return;
    }
    touch(*instance);
    Event event{};
    event.binding = input.binding;
    event.tick = now;
    event.kind = EventKind::clientStateChanged;
    event.sourceGeneration = input.sourceGeneration;
    event.clientMessageSequence = input.clientMessageSequence;
    event.payloadBytes = input.payloadBytes;
    event.activityStateRevision = input.state.activityStateRevision;
    event.membershipRevision = input.state.membershipRevision;
    event.regionIndex = input.state.region.index;
    event.regionSliceSetHash = input.state.region.hash;
    event.currentRegionIndex = input.state.currentRegion.index;
    event.clientStateHasCurrentRegion = input.state.hasCurrentRegion;
    event.heldRegionIndex = input.state.heldRegion;
    event.teleportSliceSetIndex = input.state.teleportSliceSetIndex;
    event.teleportSliceSetHash = input.state.teleportSliceSetHash;
    event.spawnState = input.state.spawnState;
    event.teleportState = input.state.teleportState;
    event.clientStateHasRegion = input.state.hasRegion;
    event.clientStateHasSpawn = input.state.hasSpawn;
    event.clientStateHasTeleport = input.state.hasTeleport;
    append_event(event);
    append_mission_input(event, nullptr);
    instance->view.lastEventSequence = g_sequence;
}

/** Publishes one committed simulation-entity slot request without deriving readiness state. */
void apply_entity_slots_requested(const EntitySlotsRequestedInput& input,
                                  std::uint64_t now) noexcept {
    Instance* const instance = find_instance(input.binding);
    if (instance == nullptr || !instance->view.active) {
        ++g_droppedIngress;
        return;
    }
    touch(*instance);
    Event event{};
    event.binding = input.binding;
    event.tick = now;
    event.kind = EventKind::entitySlotsRequested;
    event.stateRevision = instance->view.stateRevision;
    event.sourceGeneration = input.sourceGeneration;
    event.clientMessageSequence = input.clientMessageSequence;
    event.clientMessageType = middleware::bap::activity_message::entity_slot_request::kMessageType;
    event.requestedEntitySlots = input.requestedCount;
    append_event(event);
    append_mission_input(event, nullptr);
    instance->view.lastEventSequence = g_sequence;
}

/** Applies one operator transition only when its output slot is free. */
void apply_control(const ControlRequest& request, std::uint64_t now) noexcept {
    Event event{};
    event.binding = request.binding;
    event.tick = now;
    event.lifetimeState = request.lifetimeState;
    Instance* const instance = find_instance(request.binding);
    if (instance == nullptr || !instance->view.active) {
        event.kind = EventKind::operatorRefused;
        ++g_refusedControls;
    } else if (instance->view.outputPending
               || instance->view.stateRevision == (std::numeric_limits<std::uint64_t>::max)()) {
        event.kind = EventKind::operatorRefused;
        event.stateRevision = instance->view.stateRevision;
        ++g_refusedControls;
    } else {
        touch(*instance);
        ++instance->view.stateRevision;
        instance->view.lifetimeState = request.lifetimeState;
        instance->view.lastOutputAttemptTick = 0;
        instance->view.lastOutputSourceGeneration = 0;
        instance->view.outputAttempts = 0;
        instance->view.outputStatus = OutputStatus::pending;
        instance->view.outputKind = OutputKind::authState;
        instance->view.outputPending = true;
        event.kind = EventKind::authStateCommitted;
        event.stateRevision = instance->view.stateRevision;
    }
    append_event(event);
    if (instance != nullptr) {
        instance->view.lastEventSequence = g_sequence;
    }
}

} // namespace detail

using namespace detail;

/** Queues one post-commit client State after-image for the ordered Host service slice. */
bool submit_client_state_change(const ClientStateChangeInput& input) noexcept {
    // A report that moved no region, spawn or teleport field is the client's settle, sent once
    // spawn-in completes. The mission surface needs it to time the opening line, so only a
    // malformed report is refused, never a material-less one.
    if (!state::activity::binding_matches(input.binding) || input.sourceGeneration == 0
        || input.clientMessageSequence == 0 || !input.state.committed
        || (input.state.hasRegion && input.state.region.index < 0)
        || input.state.activityStateRevision == state::activity::kInvalidRevision) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    PendingInput pending{};
    pending.kind = PendingKind::clientStateChange;
    pending.clientStateChange = input;
    if (!append_pending(pending)) {
        ++g_droppedIngress;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedIngress;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Queues one committed msg-20 simulation-entity slot request. */
bool submit_entity_slots_requested(const EntitySlotsRequestedInput& input) noexcept {
    if (!state::activity::binding_matches(input.binding) || input.sourceGeneration == 0
        || input.clientMessageSequence == 0 || input.requestedCount <= 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    PendingInput pending{};
    pending.kind = PendingKind::entitySlotsRequested;
    pending.entitySlotsRequested = input;
    if (!append_pending(pending)) {
        ++g_droppedIngress;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedIngress;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Queues one operator Auth-state transition for the exact activity generation. */
bool request_auth_state(const state::activity::SessionBinding& binding,
                        std::uint8_t lifetimeState) noexcept {
    if (!lifetime_allowed(lifetimeState) || !state::activity::binding_matches(binding)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const Instance* const instance = find_instance(binding);
    if (has_queued_control(binding)
        || (instance != nullptr
            && (instance->view.outputPending || instance->view.scriptableReservationPending))) {
        ++g_refusedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    PendingInput pending{};
    pending.kind = PendingKind::authControl;
    pending.control = {binding, lifetimeState};
    if (!append_pending(pending)) {
        ++g_refusedControls;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedControls;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Applies queued client and operator events on the server service slice. */
void service(std::uint64_t now) noexcept {
    std::array<state::activity::SessionBinding, state::activity::kSessionCapacity> bindings{};
    std::size_t bindingCount = 0;
    static_cast<void>(state::activity::snapshot_retained_bindings(bindings, bindingCount));

    AcquireSRWLockExclusive(&g_lock);
    for (Instance& instance : g_instances) {
        bool active = false;
        for (std::size_t index = 0; index < bindingCount; ++index) {
            if (same_binding(instance.view.binding, bindings[index])) {
                active = true;
                break;
            }
        }
        instance.view.active = instance.occupied && active;
        if (instance.occupied && !instance.view.active && instance.view.outputPending) {
            cancel_output(instance, now);
        }
        if (instance.occupied && !instance.view.active
            && instance.view.scriptableReservationPending) {
            instance.scriptableReservation = {};
            instance.view.scriptableReservedRevision = 0;
            instance.view.scriptableReservationPending = false;
        }
    }
    for (std::size_t index = 0; index < bindingCount; ++index) {
        Instance* const instance = ensure_instance(bindings[index]);
        if (instance != nullptr) {
            instance->view.active = true;
            touch(*instance);
        }
    }
    while (g_pendingRead < g_pending.size()) {
        const PendingInput pending = g_pending[g_pendingRead];
        ++g_pendingRead;
        if (pending.kind == PendingKind::discardedControl) {
            continue;
        }
        if (pending.kind == PendingKind::authControl) {
            --g_queuedControls;
            apply_control(pending.control, now);
        } else if (pending.kind == PendingKind::incidentControl) {
            --g_queuedControls;
            apply_incident_control(pending.incidentControl, now);
        } else if (pending.kind == PendingKind::scriptableControl) {
            --g_queuedControls;
            apply_scriptable_control(pending.scriptableControl, now);
        } else if (pending.kind == PendingKind::incident) {
            --g_queuedIngress;
            apply_incident(pending.incident, now);
        } else if (pending.kind == PendingKind::clientStateChange) {
            --g_queuedIngress;
            apply_client_state_change(pending.clientStateChange, now);
        } else if (pending.kind == PendingKind::entitySlotsRequested) {
            --g_queuedIngress;
            apply_entity_slots_requested(pending.entitySlotsRequested, now);
        } else if (pending.kind == PendingKind::clientMessage) {
            --g_queuedIngress;
            apply_client_message(pending.clientMessage, now);
        } else {
            --g_queuedIngress;
            apply_sense(pending.sense, now);
        }
    }
    g_pending.clear();
    g_pendingRead = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the latest complete diagnostic view. */
void snapshot(DiagnosticsSnapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    for (const Instance& instance : g_instances) {
        if (instance.occupied && output.instanceCount < output.instances.size()) {
            output.instances[output.instanceCount] = instance.view;
            ++output.instanceCount;
        }
    }
    for (std::size_t index = 0; index < g_eventCount; ++index) {
        output.events[index] = g_events[(g_eventStart + index) % g_events.size()];
    }
    output.eventCount = g_eventCount;
    snapshot_incidents(output);
    snapshot_client_messages(output);
    output.droppedIngress = g_droppedIngress;
    output.queuedControls = g_queuedControls;
    output.refusedControls = g_refusedControls;
    output.overwrittenEvents = g_overwrittenEvents;
    ReleaseSRWLockShared(&g_lock);
}

/** Copies one exact activity generation without exposing the Host lock. */
bool instance_snapshot(const state::activity::SessionBinding& binding,
                       InstanceSnapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool found = instance != nullptr;
    if (found) {
        output = instance->view;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Reads the current feed position without replaying retained history. */
EventCursor current_event_cursor() noexcept {
    AcquireSRWLockShared(&g_lock);
    const EventCursor cursor{g_eventGeneration, g_sequence};
    ReleaseSRWLockShared(&g_lock);
    return cursor;
}

/** Copies retained events after one cursor and reports reset or overwrite gaps. */
void read_events_after(EventCursor after, EventRead& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    output.cursor = {g_eventGeneration, g_sequence};
    output.reset = after.generation != g_eventGeneration;
    if (g_eventCount != 0) {
        const std::uint64_t afterSequence = output.reset ? 0 : after.sequence;
        const std::uint64_t oldestSequence = g_events[g_eventStart].sequence;
        std::size_t first = 0;
        if (afterSequence < oldestSequence) {
            const std::uint64_t retainedPredecessor = oldestSequence - 1;
            if (afterSequence < retainedPredecessor) {
                output.gap = true;
                output.missed = retainedPredecessor - afterSequence;
            }
        } else {
            while (first < g_eventCount
                   && g_events[(g_eventStart + first) % g_events.size()].sequence
                          <= afterSequence) {
                ++first;
            }
        }
        for (; first < g_eventCount; ++first) {
            output.events[output.count++] = g_events[(g_eventStart + first) % g_events.size()];
        }
    }
    ReleaseSRWLockShared(&g_lock);
}

/** Reads the committed Auth state for one exact activity generation. */
bool auth_state(const state::activity::SessionBinding& binding, AuthState& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool found = instance != nullptr;
    if (found) {
        output.revision = instance->view.stateRevision;
        output.lifetimeState = instance->view.lifetimeState;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

/** Records one refused BAP attempt without clearing the committed output. */
void note_auth_attempt(const state::activity::SessionBinding& binding,
                       std::uint64_t sourceGeneration,
                       std::uint64_t revision,
                       std::uint8_t lifetimeState,
                       OutputStatus status) noexcept {
    if (revision == 0 || status == OutputStatus::idle || status == OutputStatus::pending
        || status == OutputStatus::transportStaged || status == OutputStatus::canceled) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    if (instance != nullptr && instance->view.outputPending
        && instance->view.outputKind == OutputKind::authState
        && instance->view.stateRevision == revision
        && instance->view.lifetimeState == lifetimeState) {
        instance->view.lastOutputAttemptTick = GetTickCount64();
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = status;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Marks one committed Auth revision as staged into the transport output queue. */
void note_auth_transport_staged(const state::activity::SessionBinding& binding,
                                std::uint64_t sourceGeneration,
                                std::uint64_t revision,
                                std::uint8_t lifetimeState) noexcept {
    if (revision == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    if (instance != nullptr && instance->view.outputPending
        && instance->view.outputKind == OutputKind::authState
        && instance->view.stateRevision == revision
        && instance->view.lifetimeState == lifetimeState) {
        instance->view.transportRevision = revision;
        instance->view.lastOutputAttemptTick = GetTickCount64();
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = OutputStatus::transportStaged;
        instance->view.outputPending = false;
        instance->view.outputKind = OutputKind::none;
        Event event{};
        event.binding = binding;
        event.tick = instance->view.lastOutputAttemptTick;
        event.kind = EventKind::authStateTransportStaged;
        event.sourceGeneration = sourceGeneration;
        event.stateRevision = revision;
        event.lifetimeState = lifetimeState;
        append_event(event);
        instance->view.lastEventSequence = g_sequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Clears every instance, queue, and diagnostic counter. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_eventGeneration = next_nonzero(g_eventGeneration);
    for (Instance& instance : g_instances) {
        clear_instance(instance);
    }
    std::vector<PendingInput>{}.swap(g_pending);
    for (Event& event : g_events) {
        event = {};
    }
    reset_mission_inputs();
    reset_incidents();
    reset_client_messages();
    g_pendingRead = 0;
    g_queuedIngress = 0;
    g_queuedControls = 0;
    g_eventStart = 0;
    g_eventCount = 0;
    g_sequence = 0;
    g_scriptableReservationGeneration = next_nonzero(g_scriptableReservationGeneration);
    g_scriptableReservationSequence = 0;
    g_touch = 0;
    g_droppedIngress = 0;
    g_refusedControls = 0;
    g_overwrittenEvents = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::activity::host
