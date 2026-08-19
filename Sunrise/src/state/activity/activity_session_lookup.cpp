#include <Windows.h>

#include <limits>

#include "../runtime/storage/internal.h"
#include "runtime.h"
#include "transactions/internal.h"

namespace sunrise::state::activity {
namespace {

/** @return True when every scalar and captured descriptor field is identical. */
[[nodiscard]] bool same_destination(const destination::DestinationSelection& left,
                                    const destination::DestinationSelection& right) noexcept {
    return left.packageName == right.packageName
           && left.packageNameLength == right.packageNameLength && left.reason == right.reason
           && left.previousActivityIndex == right.previousActivityIndex
           && left.activityIndex == right.activityIndex && left.elementIndex == right.elementIndex
           && left.arrivalBubbleHash == right.arrivalBubbleHash
           && left.spawnSetHash == right.spawnSetHash
           && left.hasElementIndex == right.hasElementIndex
           && left.hasArrivalBubbleHash == right.hasArrivalBubbleHash
           && left.hasSpawnSetHash == right.hasSpawnSetHash
           && left.arrivalBubbleOverride == right.arrivalBubbleOverride
           && left.hasArrivalBubbleOverride == right.hasArrivalBubbleOverride
           && left.sliceSetOverride == right.sliceSetOverride
           && left.hasSliceSetOverride == right.hasSliceSetOverride
           && left.spawnSetOverride == right.spawnSetOverride
           && left.hasSpawnSetOverride == right.hasSpawnSetOverride
           && left.descriptorBits == right.descriptorBits
           && left.descriptorBitLength == right.descriptorBitLength
           && left.descriptorNameBit == right.descriptorNameBit
           && left.hasDescriptorName == right.hasDescriptorName;
}

/** @return True when a record is the exact generation named by one binding. */
[[nodiscard]] bool record_matches(const SessionRecord& record,
                                  const SessionBinding& binding) noexcept {
    return record.occupied && binding.sessionId != kAbsentSessionId
           && binding.createdRevision != kInvalidRevision && record.sessionId == binding.sessionId
           && record.createdRevision == binding.createdRevision
           && same_destination(record.destination, binding.destination);
}

} // namespace

/** Tests whether a nonzero activity-session id is still in the bounded table. */
bool contains(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool found = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Tests whether a committed activity-session id has finished a join. */
bool is_joined(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool joined = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            joined = record.joined && record.joinedRevision != kInvalidRevision;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return joined;
}

/** Copies the exact destination and generation of one committed session. */
bool snapshot_binding(std::uint64_t sessionId, SessionBinding& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, sessionId);
    const bool found = slot < kSessionCapacity;
    if (found) {
        const SessionRecord& record = state.sessions[slot];
        output.destination = record.destination;
        output.sessionId = record.sessionId;
        output.createdRevision = record.createdRevision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Tests whether the exact bound record generation is still committed. */
bool binding_matches(const SessionBinding& binding) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    const bool matches = slot < kSessionCapacity && record_matches(state.sessions[slot], binding);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return matches;
}

/** Retains an exact record generation against release and allocator eviction. */
bool retain_binding(const SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    bool retained = slot < kSessionCapacity && record_matches(state.sessions[slot], binding);
    if (retained) {
        SessionRecord& record = state.sessions[slot];
        retained = record.bindingRetainCount
                   != (std::numeric_limits<decltype(record.bindingRetainCount)>::max)();
        if (retained) {
            ++record.bindingRetainCount;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return retained;
}

/** Releases one retain only when the exact record generation still matches. */
void release_binding(const SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    if (slot < kSessionCapacity && record_matches(state.sessions[slot], binding)
        && state.sessions[slot].bindingRetainCount != 0) {
        --state.sessions[slot].bindingRetainCount;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity
