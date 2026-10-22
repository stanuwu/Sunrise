#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "../../runtime/storage/internal.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::state::activity::mission {
namespace {

/** An early publication may bind only the exact output owned by the current durable head. */
[[nodiscard]] bool publication_owns_pending(const MissionState& state,
                                            const DeviceRequestReport& request,
                                            const DevicePublication& publication) noexcept {
    if (state.pendingIntents.empty() || publication.originatingHostRevision == 0) {
        return false;
    }
    const auto& head = state.pendingIntents.front();
    return head.hostOutputRevision == publication.originatingHostRevision
           && head.value.kind == IntentKind::setDeviceChannel
           && head.value.requestKey == request.requestKey && head.value.firstRow == request.slotRow
           && head.value.deviceChannel == request.channel && head.value.deviceValue == request.value
           && head.value.attemptGeneration == request.attemptGeneration;
}

/** Acknowledgement cannot erase a newer publication or a report that already confirmed it. */
[[nodiscard]] bool same_device_confirmation(const DeviceRequestReport& left,
                                            const DeviceRequestReport& right) noexcept {
    return left.requestKey == right.requestKey && left.attemptGeneration == right.attemptGeneration
           && left.slotRow == right.slotRow && left.channel == right.channel
           && left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotIndex == right.slotIndex && left.sequence == right.sequence
           && left.value == right.value && left.sourceGeneration >= right.sourceGeneration;
}

/**
 * Only a newer owner with the exact desired channel can replace report eligibility, and only once
 * the link that owns the request is gone, as when its client reattaches. While the owner is live,
 * every other link that receives the same body only observes the device; when the device's bubble
 * changes holder, the new holder's admitted report takes the request (`observe_device_report`).
 */
[[nodiscard]] const DevicePublication*
matching_device_publication(const MissionState& state,
                            const DeviceRequestReport& request,
                            const DevicePublicationBoundary& boundary,
                            std::span<const DevicePublication> publications) noexcept {
    const auto live = std::span(boundary.liveGenerations)
                          .first((std::min)(static_cast<std::size_t>(boundary.liveGenerationCount),
                                            boundary.liveGenerations.size()));
    if (request.attemptGeneration != boundary.attemptGeneration
        || request.sourceGeneration >= boundary.sourceGeneration
        || request.channel >= kDeviceChannelCount
        || (request.sourceGeneration != 0
            && std::find(live.begin(), live.end(), request.sourceGeneration) != live.end())) {
        return nullptr;
    }
    for (const auto& publication : publications) {
        const auto& channel = publication.channels[request.channel];
        if (!channel.valueKnown || !channel.sequenceKnown || channel.sequence <= 0
            || channel.value != request.value) {
            continue;
        }
        if (request.sourceGeneration != 0
                ? request.objectTag != publication.objectTag
                      || request.registryKey != publication.registryKey
                      || request.slotIndex != publication.slotIndex
                      || request.sequence != channel.sequence
                : !publication_owns_pending(state, request, publication)) {
            continue;
        }
        return &publication;
    }
    return nullptr;
}

/** Retires an exact durable head that never owns a Host output revision. */
[[nodiscard]] Status retire_unassigned_intent(const SessionBinding& binding,
                                              const ProgramKey& program,
                                              std::uint64_t expectedMissionRevision,
                                              std::uint64_t expectedIntentSequence,
                                              bool accepted,
                                              Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    if (expectedIntentSequence == kAbsentIntentSequence) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    PendingIntent* pending = nullptr;
    if (status == Status::ready) {
        status = checked_head(
            *record, program, expectedMissionRevision, expectedIntentSequence, pending);
    }
    if (status == Status::ready && pending->hostOutputRevision != kAbsentHostOutputRevision) {
        status = Status::hostRevisionMismatch;
    }
    const bool restart = status == Status::ready && accepted
                         && pending->value.kind == IntentKind::restartCheckpoint
                         && pending->value.checkpointReleaseRequest == 0;
    if (restart
        && (pending->value.attemptGeneration != record->mission.attempt.generation
            || record->mission.attempt.generation == (std::numeric_limits<std::uint64_t>::max)())) {
        status = Status::invalidTransition;
    }
    if (status == Status::ready) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            if (restart) {
                ++candidate.attempt.generation;
                candidate.attempt.complete = false;
                candidate.timers = {};
                candidate.timerCount = 0;
                candidate.deviceRequests.clear();
                candidate.squadPopulations.clear();
            }
            candidate.pendingIntents.erase(candidate.pendingIntents.begin());
            if (!publish(activity, *record)) {
                status = Status::revisionExhausted;
            } else {
                record->mission = std::move(candidate);
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

} // namespace

/** Assigns the exact next Host output revision to the durable head intent. */
Status assign_intent_output(const SessionBinding& binding,
                            const ProgramKey& program,
                            std::uint64_t expectedMissionRevision,
                            std::uint64_t expectedIntentSequence,
                            std::uint64_t hostOutputRevision,
                            Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    if (expectedIntentSequence == kAbsentIntentSequence
        || hostOutputRevision == kAbsentHostOutputRevision) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    PendingIntent* pending = nullptr;
    if (status == Status::ready) {
        status = checked_head(
            *record, program, expectedMissionRevision, expectedIntentSequence, pending);
    }
    if (status == Status::ready && record->mission.faulted) {
        status = Status::invalidTransition;
    }
    if (status == Status::ready && pending->hostOutputRevision != kAbsentHostOutputRevision
        && pending->hostOutputRevision != hostOutputRevision) {
        status = Status::hostRevisionMismatch;
    }
    // A fresh assignment must leave one Activity State revision for its exact release or ack.
    if (status == Status::ready && pending->hostOutputRevision == kAbsentHostOutputRevision
        && activity.stateRevision >= kMaximumRevision - 1) {
        status = Status::revisionExhausted;
    }
    if (status == Status::ready && pending->hostOutputRevision == kAbsentHostOutputRevision) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            candidate.pendingIntents[0].hostOutputRevision = hostOutputRevision;
            if (!publish(activity, *record)) {
                status = Status::revisionExhausted;
            } else {
                record->mission = std::move(candidate);
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Reads whether the exact durable head still owns one queued Host output revision. */
bool intent_output_assigned(const SessionBinding& binding,
                            std::uint64_t expectedIntentSequence,
                            std::uint64_t expectedHostOutputRevision) noexcept {
    if (expectedIntentSequence == kAbsentIntentSequence
        || expectedHostOutputRevision == kAbsentHostOutputRevision) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& activity = runtime::storage::g_state.activity;
    const SessionRecord* const record = find_record(activity, binding);
    const bool assigned =
        record != nullptr && record->mission.programBound && !record->mission.pendingIntents.empty()
        && record->mission.pendingIntents[0].sequence == expectedIntentSequence
        && record->mission.pendingIntents[0].hostOutputRevision == expectedHostOutputRevision;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return assigned;
}

/** Releases one output assignment while retaining the durable intent and optional parent reset. */
Status release_intent_output(const SessionBinding& binding,
                             const ProgramKey& program,
                             std::uint64_t expectedMissionRevision,
                             std::uint64_t expectedIntentSequence,
                             std::uint64_t expectedHostOutputRevision,
                             Snapshot& output,
                             const SquadPopulation* preparedParent) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    if (expectedIntentSequence == kAbsentIntentSequence
        || expectedHostOutputRevision == kAbsentHostOutputRevision) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    PendingIntent* pending = nullptr;
    if (status == Status::ready) {
        status = checked_head(
            *record, program, expectedMissionRevision, expectedIntentSequence, pending);
    }
    if (status == Status::ready && pending->hostOutputRevision != expectedHostOutputRevision) {
        status = Status::hostRevisionMismatch;
    }
    if (status == Status::ready && preparedParent != nullptr
        && (pending->value.kind != IntentKind::runActorProgram || !pending->value.active
            || preparedParent->squadRow != pending->value.secondRow
            || preparedParent->attemptGeneration != pending->value.attemptGeneration
            || preparedParent->attemptGeneration != record->mission.attempt.generation)) {
        status = Status::intentMismatch;
    }
    if (status == Status::ready) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            candidate.pendingIntents[0].hostOutputRevision = kAbsentHostOutputRevision;
            if (preparedParent != nullptr) {
                status = prepare_squad_population(candidate, pending->value);
            }
            if (status == Status::ready) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Removes the durable head only after its exact Host output revision was staged. */
Status acknowledge_intent_output(const SessionBinding& binding,
                                 const ProgramKey& program,
                                 std::uint64_t expectedMissionRevision,
                                 std::uint64_t expectedIntentSequence,
                                 std::uint64_t expectedHostOutputRevision,
                                 Snapshot& output,
                                 const DeviceRequestReport* device,
                                 const SquadPopulation* population,
                                 std::span<const SquadPopulation> scenePopulations) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    if (expectedIntentSequence == kAbsentIntentSequence
        || expectedHostOutputRevision == kAbsentHostOutputRevision) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    PendingIntent* pending = nullptr;
    if (status == Status::ready) {
        status = checked_head(
            *record, program, expectedMissionRevision, expectedIntentSequence, pending);
    }
    if (status == Status::ready && pending->hostOutputRevision != expectedHostOutputRevision) {
        status = Status::hostRevisionMismatch;
    }
    if (status == Status::ready && device != nullptr
        && (pending->value.kind != IntentKind::setDeviceChannel
            || device->requestKey != pending->value.requestKey
            || device->attemptGeneration != pending->value.attemptGeneration
            || device->attemptGeneration != record->mission.attempt.generation
            || device->slotRow != pending->value.firstRow
            || device->channel != pending->value.deviceChannel
            || device->value != pending->value.deviceValue || device->channel >= kDeviceChannelCount
            || device->sourceGeneration == 0)) {
        status = Status::intentMismatch;
    }
    if (status == Status::ready) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            if (population != nullptr) {
                status = retain_squad_population(candidate, pending->value, *population);
            }
            if (status == Status::ready
                && ((pending->value.kind == IntentKind::activateAuthoredScene
                     && pending->value.active)
                    || !scenePopulations.empty())) {
                status = retain_scene_populations(candidate, pending->value, scenePopulations);
            }
            if (status == Status::ready && device != nullptr) {
                const auto found = std::find_if(candidate.deviceRequests.begin(),
                                                candidate.deviceRequests.end(),
                                                [device](const auto& existing) noexcept {
                                                    return existing.slotRow == device->slotRow
                                                           && existing.channel == device->channel;
                                                });
                if (found != candidate.deviceRequests.end()) {
                    if (found->requestKey <= device->requestKey
                        && !same_device_confirmation(*found, *device)) {
                        *found = *device;
                    }
                } else {
                    try {
                        candidate.deviceRequests.push_back(*device);
                    } catch (const std::bad_alloc&) {
                        status = Status::outOfMemory;
                    }
                }
            }
            if (pending->value.kind == IntentKind::setLifetime
                && pending->value.lifetimeState == kCompletedLifetimeState
                && pending->value.attemptGeneration == candidate.attempt.generation) {
                candidate.attempt.complete = true;
            }
            candidate.pendingIntents.erase(candidate.pendingIntents.begin());
            if (status == Status::ready) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Transport publication changes report ownership without replaying the public request. */
Status renew_device_publications(const SessionBinding& binding,
                                 const DevicePublicationBoundary& boundary,
                                 std::span<const DevicePublication> publications) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto* record = find_record(activity, binding);
    if (record == nullptr || !record->mission.programBound || record->mission.faulted
        || record->mission.attempt.complete
        || record->mission.attempt.generation != boundary.attemptGeneration || publications.empty()
        || record->mission.deviceRequests.empty()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return Status::ready;
    }
    Status status = boundary.sourceGeneration == 0
                            || boundary.inputSequence > record->mission.issuedInputSequence
                        ? Status::invalidTransition
                        : Status::ready;
    for (const auto& publication : publications) {
        for (const auto& channel : publication.channels) {
            if ((channel.valueKnown
                 && (!std::isfinite(channel.value) || channel.value < 0.0F || channel.value > 1.0F))
                || (channel.sequenceKnown
                    && (channel.sequence < -1
                        || channel.sequence > (std::numeric_limits<std::int16_t>::max)()))) {
                status = Status::invalidTransition;
            }
        }
    }
    const bool needed =
        status == Status::ready
        && std::any_of(record->mission.deviceRequests.begin(),
                       record->mission.deviceRequests.end(),
                       [&](const auto& request) {
                           return matching_device_publication(
                                      record->mission, request, boundary, publications)
                                  != nullptr;
                       });
    if (!needed) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return status;
    }
    MissionState candidate{};
    if (status == Status::ready && !copy_mission_state(record->mission, candidate)) {
        status = Status::outOfMemory;
    }
    bool changed = false;
    if (status == Status::ready) {
        for (auto& request : candidate.deviceRequests) {
            const auto* publication =
                matching_device_publication(candidate, request, boundary, publications);
            if (publication != nullptr) {
                const auto& channel = publication->channels[request.channel];
                request.objectTag = publication->objectTag;
                request.registryKey = publication->registryKey;
                request.slotIndex = publication->slotIndex;
                request.sequence = static_cast<std::int16_t>(channel.sequence);
                request.sourceGeneration = boundary.sourceGeneration;
                request.inputSequenceAtStage = boundary.inputSequence;
                request.clientMessageSequenceAtStage = boundary.clientMessageSequence;
                request.reported = {};
                request.applied = false;
                changed = true;
            }
        }
        if (changed) {
            if (!publish(activity, *record)) {
                status = Status::revisionExhausted;
            } else {
                record->mission = std::move(candidate);
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/**
 * Joins only fresh accepted deltas to a staged device request from the same attempt. The link that
 * simulates the device reports for it, so an admitted report from another link takes over a
 * request that is not yet applied: that link's client now holds the device's bubble. Its own
 * reports start the join again; the previous owner's partial channels do not count.
 * @param binding Exact activity owning the report.
 * @param program Current mission program identity.
 * @param report Accepted sparse channel values with their intake owners.
 * @param appliedRequests Receives each request first satisfied by this report.
 * @param output Receives the current durable mission state on success.
 * @return Ready when the report joined or took over its requests, or was ignored.
 */
Status observe_device_report(const SessionBinding& binding,
                             const ProgramKey& program,
                             const DeviceReport& report,
                             std::array<std::uint64_t, kDeviceChannelCount>& appliedRequests,
                             Snapshot& output) noexcept {
    appliedRequests = {};
    output = {};
    for (const auto& channel : report.channels) {
        if ((channel.valueKnown
             && (!std::isfinite(channel.value) || channel.value < 0.0F || channel.value > 1.0F))
            || (channel.sequenceKnown && channel.sequence < -1)) {
            return Status::invalidTransition;
        }
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto* record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    if (status == Status::ready
        && (!record->mission.programBound || !same_program(record->mission.program, program))) {
        status = Status::programMismatch;
    }
    if (status == Status::ready
        && (record->mission.faulted || report.inputSequence == 0
            || report.inputSequence > record->mission.issuedInputSequence)) {
        status = Status::invalidTransition;
    }
    if (status == Status::ready && report.attemptGeneration == record->mission.attempt.generation) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            bool changed = false;
            for (auto& request : candidate.deviceRequests) {
                if (request.applied || request.sourceGeneration == 0
                    || request.attemptGeneration != report.attemptGeneration
                    || request.objectTag != report.objectTag
                    || request.registryKey != report.registryKey
                    || request.slotIndex != report.slotIndex
                    || request.channel >= report.channels.size()) {
                    continue;
                }
                if (request.sourceGeneration != report.sourceGeneration) {
                    if (!report.admitted || report.sourceGeneration == 0) {
                        continue;
                    }
                    request.sourceGeneration = report.sourceGeneration;
                    request.reported = {};
                    changed = true;
                }
                if (report.inputSequence <= request.inputSequenceAtStage
                    || report.clientMessageSequence <= request.clientMessageSequenceAtStage) {
                    continue;
                }
                const auto before = request.reported;
                const auto& incoming = report.channels[request.channel];
                if (incoming.valueKnown) {
                    request.reported.value = incoming.value;
                    request.reported.valueKnown = true;
                }
                if (incoming.sequenceKnown) {
                    request.reported.sequence = incoming.sequence;
                    request.reported.sequenceKnown = true;
                }
                changed = changed || before != request.reported;
                if (request.reported.valueKnown && request.reported.sequenceKnown
                    && request.reported.sequence == request.sequence
                    && request.reported.value == request.value) {
                    request.applied = true;
                    changed = true;
                    appliedRequests[request.channel] = request.requestKey;
                }
            }
            if (changed) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    if (status != Status::ready) {
        appliedRequests = {};
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Removes one successfully applied local effect that owns no Host output revision. */
Status acknowledge_intent(const SessionBinding& binding,
                          const ProgramKey& program,
                          std::uint64_t expectedMissionRevision,
                          std::uint64_t expectedIntentSequence,
                          Snapshot& output) noexcept {
    return retire_unassigned_intent(
        binding, program, expectedMissionRevision, expectedIntentSequence, true, output);
}

/** Drops the durable head after a request was refused. */
Status discard_intent(const SessionBinding& binding,
                      const ProgramKey& program,
                      std::uint64_t expectedMissionRevision,
                      std::uint64_t expectedIntentSequence,
                      Snapshot& output) noexcept {
    return retire_unassigned_intent(
        binding, program, expectedMissionRevision, expectedIntentSequence, false, output);
}
} // namespace sunrise::state::activity::mission
