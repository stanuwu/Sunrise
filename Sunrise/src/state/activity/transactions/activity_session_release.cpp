#include "../../runtime/storage/internal.h"
#include "../runtime.h"
#include "internal.h"

namespace sunrise::state::activity {

/** Frees one committed activity-session record. */
bool release_session(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t slot = transactions::find_session(state, sessionId);
    const bool released = slot < kSessionCapacity && state.sessions[slot].bindingRetainCount == 0;
    if (released) {
        // The revision bump retires every plan prepared against this record.
        state.sessions[slot] = {};
        ++state.stateRevision;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return released;
}

} // namespace sunrise::state::activity
