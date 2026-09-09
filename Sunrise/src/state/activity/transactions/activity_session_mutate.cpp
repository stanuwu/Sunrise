#include "../../runtime/storage/internal.h"
#include "internal.h"

namespace sunrise::state::activity {

/** Applies a script-declared slice-set override to an active session's destination. */
bool override_destination_slice_set(std::uint64_t sessionId, std::uint16_t sliceSet) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = transactions::find_session(state, sessionId);
    if (target == kInvalidSessionSlot) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    SessionRecord& record = state.sessions[target];
    if (record.destination.sliceSetOverride == sliceSet && record.destination.hasSliceSetOverride) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return true;
    }
    record.destination.sliceSetOverride = sliceSet;
    record.destination.hasSliceSetOverride = true;
    ++state.stateRevision;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

} // namespace sunrise::state::activity