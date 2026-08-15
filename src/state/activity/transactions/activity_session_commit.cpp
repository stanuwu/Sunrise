#include "../../runtime/storage/internal.h"
#include "../destination/activity_destination_validation.h"
#include "../runtime.h"
#include "internal.h"

namespace sunrise::state::activity {

/** Commits one prepared activity-session allocation when its revisions still match. */
bool commit(PendingAllocation& allocation) noexcept {
    // Take the plan first so no transaction can replay, pass or fail.
    const PendingAllocation prepared = allocation;
    allocation = {};
    if (!prepared.prepared || prepared.sessionId == kAbsentSessionId
        || prepared.expectedStateRevision == kInvalidRevision
        || prepared.expectedAllocatorRevision == kInvalidRevision
        || prepared.sessionId
               != transactions::compose_session_soid(prepared.soidBase,
                                                     prepared.expectedNextSessionId)
        || prepared.targetSlot >= kSessionCapacity || !destination::valid(prepared.destination)) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    if (!transactions::allocation_available(state)
        || state.stateRevision != prepared.expectedStateRevision
        || state.allocatorRevision != prepared.expectedAllocatorRevision
        || state.nextSessionId != prepared.expectedNextSessionId) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    // Pick again under the write lock so stale plan data cannot redirect State.
    const std::size_t target = transactions::select_target(state);
    if (target != prepared.targetSlot) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    SessionRecord& record = state.sessions[target];
    // A full reset clears the evicted record and restores each field's own unset value.
    record = {};
    ++state.stateRevision;
    record.destination = prepared.destination;
    record.sessionId = prepared.sessionId;
    record.createdRevision = state.stateRevision;
    record.recordRevision = state.stateRevision;
    record.occupied = true;
    transactions::advance_allocator(state);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

} // namespace sunrise::state::activity
