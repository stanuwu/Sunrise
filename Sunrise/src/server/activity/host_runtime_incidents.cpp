/**
 * Client and operator incident retention, and its transport staging record.
 * Helpers here need the Host runtime lock; the entry points take it themselves.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../state/activity/runtime.h"
#include "host_runtime_internal.h"

namespace sunrise::server::activity::host {
namespace {

using namespace detail;

std::array<IncidentRecord, kIncidentHistoryCapacity> g_incidents{};
std::uint64_t g_droppedIncidents{};
std::uint64_t g_refusedIncidents{};
std::uint64_t g_overwrittenIncidents{};

/** @return True when the shared codec accepts every bounded outer incident field. */
[[nodiscard]] bool
incident_allowed(const middleware::bap::activity_message::incident::Incident& incident) noexcept {
    return middleware::bap::activity_message::incident::outer_valid(incident);
}

/** Reserves a full incident record without discarding an unstaged operator event. */
[[nodiscard]] IncidentRecord* reserve_incident_record() noexcept {
    IncidentRecord* selected = nullptr;
    for (IncidentRecord& record : g_incidents) {
        if (record.sequence == 0) {
            return &record;
        }
        const bool evictable = !record.outbound || record.transportStages != 0
                               || record.status == IncidentStatus::canceled;
        if (evictable && (selected == nullptr || record.sequence < selected->sequence)) {
            selected = &record;
        }
    }
    if (selected != nullptr) {
        ++g_overwrittenIncidents;
    }
    return selected;
}

} // namespace

namespace detail {

/** Finds one retained outbound incident revision while the runtime lock is held. */
[[nodiscard]] IncidentRecord* find_incident(const state::activity::SessionBinding& binding,
                                            std::uint64_t revision) noexcept {
    for (IncidentRecord& record : g_incidents) {
        if (record.sequence != 0 && record.outbound && record.revision == revision
            && same_binding(record.binding, binding)) {
            return &record;
        }
    }
    return nullptr;
}

/** Copies one incident summary into the chronological event history. */
void fill_incident_event(Event& event,
                         const middleware::bap::activity_message::incident::Incident& incident,
                         std::uint64_t revision) noexcept {
    event.incidentRevision = revision;
    event.incidentTarget = incident.primaryTarget;
    event.incidentExtraTargets = incident.extraTargetCount;
    event.incidentSelectorBytes = incident.selectorLength;
    event.incidentPayloadBytes = incident.payloadLength;
}

/** Retains one outer-valid client incident for inspection and parsed-field replay. */
void apply_incident(const IncidentInput& input, std::uint64_t now) noexcept {
    Instance* const instance = find_instance(input.binding);
    if (instance == nullptr || !instance->view.active) {
        ++g_droppedIngress;
        ++g_droppedIncidents;
        return;
    }
    touch(*instance);
    ++instance->view.incidentsReceived;
    Event event{};
    event.attemptGeneration = input.attemptGeneration;
    event.binding = input.binding;
    event.tick = now;
    event.kind = EventKind::incidentReceived;
    event.sourceGeneration = input.sourceGeneration;
    event.clientMessageSequence = input.clientMessageSequence;
    event.payloadBytes = input.payloadBytes;
    fill_incident_event(event, input.incident, 0);
    event.hasPlayerTrigger = input.hasPlayerTrigger;
    if (input.hasPlayerTrigger) {
        event.playerTriggerRegistryKey = input.playerTrigger.registryKey;
        event.playerTriggerSlotType = input.playerTrigger.slotType;
        event.playerTriggerSlotIndex = input.playerTrigger.slotIndex;
        event.playerTriggerResolvedObjectId = input.playerTrigger.resolvedObjectId;
    }
    event.hasCinematic = input.hasCinematic;
    if (input.hasCinematic) {
        event.cinematicRegistryKey = input.cinematic.registryKey;
        event.cinematicSlotType = input.cinematic.slotType;
        event.cinematicSlotIndex = input.cinematic.slotIndex;
        event.cinematicRuntimeObjectId = input.cinematic.runtimeObjectId;
        event.cinematicEventValue = input.cinematic.eventValue;
        event.cinematicSignal = input.cinematicSignal;
    }
    append_event(event);
    append_mission_input(event, nullptr);
    instance->view.lastEventSequence = g_sequence;

    IncidentRecord* const record = reserve_incident_record();
    if (record == nullptr) {
        ++g_droppedIncidents;
        return;
    }
    *record = {};
    record->binding = input.binding;
    record->incident = input.incident;
    record->sequence = g_sequence;
    record->tick = now;
    record->lastSourceGeneration = input.sourceGeneration;
    record->clientMessageSequence = input.clientMessageSequence;
    record->payloadBytes = input.payloadBytes;
    record->status = IncidentStatus::received;
}

/** Commits one operator incident into the per-generation ordered output history. */
void apply_incident_control(const IncidentRequest& request, std::uint64_t now) noexcept {
    Event event{};
    event.binding = request.binding;
    event.tick = now;
    event.kind = EventKind::incidentRefused;
    fill_incident_event(event, request.incident, 0);
    Instance* const instance = find_instance(request.binding);
    IncidentRecord* record = nullptr;
    // The reserve runs last so a refusal never consumes a record slot.
    if (instance == nullptr || !instance->view.active || instance->view.outputPending
        || instance->view.incidentRevision == (std::numeric_limits<std::uint64_t>::max)()
        || (record = reserve_incident_record()) == nullptr) {
        ++g_refusedControls;
        ++g_refusedIncidents;
    } else {
        touch(*instance);
        ++instance->view.incidentRevision;
        ++instance->view.incidentsQueued;
        ++instance->view.incidentsPending;
        instance->view.outputPending = true;
        instance->view.outputKind = OutputKind::incident;
        instance->view.outputStatus = OutputStatus::pending;
        instance->view.lastOutputAttemptTick = 0;
        instance->view.lastOutputSourceGeneration = 0;
        instance->view.outputAttempts = 0;
        *record = {};
        record->binding = request.binding;
        record->incident = request.incident;
        record->revision = instance->view.incidentRevision;
        record->tick = now;
        record->status = IncidentStatus::queued;
        record->outbound = true;
        event.kind = EventKind::incidentQueued;
        fill_incident_event(event, request.incident, record->revision);
    }
    append_event(event);
    if (record != nullptr && event.kind == EventKind::incidentQueued) {
        record->sequence = g_sequence;
    }
    if (instance != nullptr) {
        instance->view.lastEventSequence = g_sequence;
    }
}

/** Copies the retained incident history and its counters into the diagnostic view. */
void snapshot_incidents(DiagnosticsSnapshot& output) noexcept {
    for (const IncidentRecord& record : g_incidents) {
        if (record.sequence != 0 && output.incidentCount < output.incidents.size()) {
            output.incidents[output.incidentCount++] = record;
        }
    }
    std::sort(output.incidents.begin(),
              output.incidents.begin() + static_cast<std::ptrdiff_t>(output.incidentCount),
              [](const IncidentRecord& first, const IncidentRecord& second) noexcept {
                  return first.sequence < second.sequence;
              });
    output.droppedIncidents = g_droppedIncidents;
    output.refusedIncidents = g_refusedIncidents;
    output.overwrittenIncidents = g_overwrittenIncidents;
}

/** Clears every retained incident and its counters. */
void reset_incidents() noexcept {
    for (IncidentRecord& incident : g_incidents) {
        incident = {};
    }
    g_droppedIncidents = 0;
    g_refusedIncidents = 0;
    g_overwrittenIncidents = 0;
}

} // namespace detail

/** Queues one owned, outer-valid client msg 19 for the Activity Host service. */
IngressRefusal submit_incident(const IncidentInput& input) noexcept {
    if (!state::activity::binding_matches(input.binding)) {
        return IngressRefusal::binding;
    }
    if (!incident_allowed(input.incident)) {
        return IngressRefusal::payload;
    }
    AcquireSRWLockExclusive(&g_lock);
    PendingInput pending{};
    pending.kind = PendingKind::incident;
    pending.incident = input;
    if (!append_pending(pending)) {
        ++g_droppedIngress;
        ++g_droppedIncidents;
        ReleaseSRWLockExclusive(&g_lock);
        return IngressRefusal::queue;
    }
    ++g_queuedIngress;
    ReleaseSRWLockExclusive(&g_lock);
    return IngressRefusal::none;
}

/** Queues one outer-valid operator msg 19 for the exact activity generation. */
bool request_incident(
    const state::activity::SessionBinding& binding,
    const middleware::bap::activity_message::incident::Incident& incident) noexcept {
    if (!incident_allowed(incident) || !state::activity::binding_matches(binding)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const Instance* const instance = find_instance(binding);
    if (has_queued_control(binding)
        || (instance != nullptr
            && (instance->view.outputPending || instance->view.scriptableReservationPending))) {
        ++g_refusedControls;
        ++g_refusedIncidents;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    PendingInput pending{};
    pending.kind = PendingKind::incidentControl;
    pending.incidentControl = {binding, incident};
    if (!append_pending(pending)) {
        ++g_refusedControls;
        ++g_refusedIncidents;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ++g_queuedControls;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Reads the one pending incident occupying the exact instance's serialized output slot. */
bool pending_incident(const state::activity::SessionBinding& binding,
                      PendingIncident& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Instance* const instance = find_instance(binding);
    const bool pending = instance != nullptr && instance->view.active
                         && instance->view.outputPending
                         && instance->view.outputKind == OutputKind::incident;
    if (pending) {
        const IncidentRecord* const record =
            find_incident(binding, instance->view.incidentRevision);
        if (record != nullptr && record->status != IncidentStatus::canceled) {
            output.incident = record->incident;
            output.revision = record->revision;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return pending && output.revision != 0;
}

/** Cancels the exact instance's raw incident before any later transport staging. */
bool cancel_pending_incident(const state::activity::SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    const bool canceled = instance != nullptr && instance->view.outputPending
                          && instance->view.outputKind == OutputKind::incident;
    if (canceled) {
        cancel_output(*instance, GetTickCount64());
    }
    ReleaseSRWLockExclusive(&g_lock);
    return canceled;
}

/** Records one failed incident attempt without clearing the serialized output slot. */
void note_incident_attempt(const state::activity::SessionBinding& binding,
                           std::uint64_t sourceGeneration,
                           std::uint64_t revision,
                           IncidentStatus status) noexcept {
    if (revision == 0
        || (status != IncidentStatus::encodeFailed && status != IncidentStatus::frameRefused)) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    IncidentRecord* const record = find_incident(binding, revision);
    if (instance != nullptr && record != nullptr && instance->view.outputPending
        && instance->view.outputKind == OutputKind::incident
        && instance->view.incidentRevision == revision) {
        record->lastAttemptTick = GetTickCount64();
        record->lastSourceGeneration = sourceGeneration;
        ++record->attempts;
        record->status = status;
        instance->view.lastOutputAttemptTick = record->lastAttemptTick;
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = OutputStatus::frameRefused;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Marks one retained incident revision as staged into a matching transport output queue. */
void note_incident_transport_staged(const state::activity::SessionBinding& binding,
                                    std::uint64_t sourceGeneration,
                                    std::uint64_t revision) noexcept {
    if (revision == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Instance* const instance = find_instance(binding);
    IncidentRecord* const record = find_incident(binding, revision);
    if (instance != nullptr && record != nullptr && instance->view.outputPending
        && instance->view.outputKind == OutputKind::incident
        && instance->view.incidentRevision == revision) {
        record->lastAttemptTick = GetTickCount64();
        record->lastSourceGeneration = sourceGeneration;
        ++record->attempts;
        ++record->transportStages;
        record->status = IncidentStatus::transportStaged;
        instance->view.incidentTransportRevision = revision;
        instance->view.lastOutputAttemptTick = record->lastAttemptTick;
        instance->view.lastOutputSourceGeneration = sourceGeneration;
        ++instance->view.outputAttempts;
        instance->view.outputStatus = OutputStatus::transportStaged;
        instance->view.outputPending = false;
        instance->view.outputKind = OutputKind::none;
        instance->view.incidentsPending = 0;
        Event event{};
        event.binding = binding;
        event.tick = record->lastAttemptTick;
        event.kind = EventKind::incidentTransportStaged;
        event.sourceGeneration = sourceGeneration;
        fill_incident_event(event, record->incident, revision);
        append_event(event);
        instance->view.lastEventSequence = g_sequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::activity::host
