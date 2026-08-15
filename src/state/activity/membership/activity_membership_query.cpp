#include "activity_membership_query.h"

#include <Windows.h>

#include "../../runtime/storage/internal.h"
#include "../transactions/internal.h"

namespace sunrise::state::activity::membership {

/** Tests whether the client has applied the current membership revision. */
bool acknowledged(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    bool applied = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        const MembershipState& membership = state.sessions[target].membership;
        applied = membership.revision != kAbsentRevision
                  && membership.acknowledgedRevision == membership.revision;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return applied;
}

/** Reads the region the client last reported it was in. */
std::int32_t reported_region(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return kAbsentRegionIndex;
    }
    std::int32_t region = kAbsentRegionIndex;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        region = state.sessions[target].membership.region.index;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return region;
}

/** Reads the identity value message 12 publishes at member record `+16`. */
std::uint64_t join_identity(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return 0;
    }
    std::uint64_t identity = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && state.sessions[target].membership.hasIdentity) {
        identity = state.sessions[target].membership.identity.joinIdentity;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return identity;
}

} // namespace sunrise::state::activity::membership
