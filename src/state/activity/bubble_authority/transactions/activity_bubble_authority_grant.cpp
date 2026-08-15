#include <Windows.h>

#include "../../../runtime/storage/internal.h"
#include "../../transactions/internal.h"
#include "../runtime.h"

namespace sunrise::state::activity::bubble_authority {

/** Picks the bubble to hand this session, if one is owed. */
bool select_grant(std::uint64_t sessionId, std::int32_t sliceSetIndex, Grant& grant) noexcept {
    grant = {};
    if (sessionId == kAbsentSessionId || sliceSetIndex < 0
        || sliceSetIndex > kMaximumGrantSliceSetIndex) {
        return false;
    }
    const auto bubble = static_cast<std::uint8_t>(sliceSetIndex >> kSliceSetToBubbleShift);
    bool owed = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && bubble < kFallbackBubble
        && state.sessions[target].bubbleAuthority.grantTokens[bubble] == 0) {
        grant.bubble = bubble;
        grant.token = kInitialGrantToken;
        owed = true;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return owed;
}

/** Records a bubble as granted so it is not granted twice. */
void record_grant(std::uint64_t sessionId, const Grant& grant) noexcept {
    if (sessionId == kAbsentSessionId || grant.bubble >= kAuthoritySlotCount || grant.token == 0) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].bubbleAuthority.grantTokens[grant.bubble] = grant.token;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity::bubble_authority
