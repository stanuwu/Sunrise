#include <Windows.h>

#include "../runtime/storage/internal.h"
#include "runtime.h"

namespace sunrise::state::activity {

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

} // namespace sunrise::state::activity
