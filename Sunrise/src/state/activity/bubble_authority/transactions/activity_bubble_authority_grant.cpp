#include <Windows.h>

#include "../../../runtime/storage/internal.h"
#include "../../transactions/internal.h"
#include "../runtime.h"

namespace sunrise::state::activity::bubble_authority {
namespace {

/** A renewed grant must differ from the client's retained token and skip zero. */
[[nodiscard]] std::uint16_t next_token(const AuthorityState& authority,
                                       std::uint8_t bubble) noexcept {
    const std::uint16_t issued = authority.issuedTokens[bubble];
    return issued < kMaximumGrantToken ? static_cast<std::uint16_t>(issued + 1)
                                       : kInitialGrantToken;
}

} // namespace

/** Picks the bubble to hand this session, if one is owed. */
bool select_grant(std::uint64_t sessionId,
                  std::int32_t sliceSetIndex,
                  Grant& grant,
                  bool enteringBubble) noexcept {
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
    if (target != kInvalidSessionSlot && bubble < kFallbackBubble) {
        const AuthorityState& authority = state.sessions[target].bubbleAuthority;
        const std::uint16_t previous = authority.grantTokens[bubble];
        if (previous == 0 || (enteringBubble && !authority.held[bubble])) {
            grant.bubble = bubble;
            grant.token = next_token(authority, bubble);
            owed = true;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return owed;
}

/** Picks the bubble a link that shares its session may be granted, if one is owed. */
bool select_shared_grant(std::uint64_t sessionId,
                         std::uint64_t generation,
                         std::int32_t sliceSetIndex,
                         std::int32_t pendingRegion,
                         bool enteringBubble,
                         Grant& grant) noexcept {
    grant = {};
    if (sessionId == kAbsentSessionId || generation == 0 || sliceSetIndex < 0
        || sliceSetIndex > kMaximumGrantSliceSetIndex) {
        return false;
    }
    const auto bubble = static_cast<std::uint8_t>(sliceSetIndex >> kSliceSetToBubbleShift);
    bool owed = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot && bubble < kFallbackBubble) {
        const AuthorityState& authority = state.sessions[target].bubbleAuthority;
        // Until its switch report lands, the releaser's placement still names the bubble while its
        // pending leg names the one it is heading to.
        const bool headingAway =
            pendingRegion >= 0 && (pendingRegion >> kSliceSetToBubbleShift) != bubble;
        const bool mayRetake =
            enteringBubble || !headingAway || authority.releasedBy[bubble] != generation;
        if (authority.grantTokens[bubble] == 0 || (!authority.held[bubble] && mayRetake)) {
            grant.bubble = bubble;
            grant.token = next_token(authority, bubble);
            owed = true;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return owed;
}

/** Records a bubble as granted so it is not granted twice. */
void record_grant(std::uint64_t sessionId, std::uint64_t generation, const Grant& grant) noexcept {
    if (sessionId == kAbsentSessionId || grant.bubble >= kAuthoritySlotCount || grant.token == 0) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        state.sessions[target].bubbleAuthority.grantTokens[grant.bubble] = grant.token;
        state.sessions[target].bubbleAuthority.issuedTokens[grant.bubble] = grant.token;
        state.sessions[target].bubbleAuthority.held[grant.bubble] = true;
        state.sessions[target].bubbleAuthority.holders[grant.bubble] = generation;
        state.sessions[target].bubbleAuthority.releasedBy[grant.bubble] = 0;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Copies which link holds each bubble of one session. */
void grant_holders(std::uint64_t sessionId, GrantHolders& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId) {
        return;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        output = state.sessions[target].bubbleAuthority.holders;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

/** Copies which link released each bubble of one session last. */
void grant_releasers(std::uint64_t sessionId, GrantHolders& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId) {
        return;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        output = state.sessions[target].bubbleAuthority.releasedBy;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

/** Releases every bubble one link holds, keeping each bubble's tokens. */
void release_holder(std::uint64_t sessionId, std::uint64_t generation) noexcept {
    if (sessionId == kAbsentSessionId || generation == 0) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        AuthorityState& authority = state.sessions[target].bubbleAuthority;
        for (std::size_t bubble = 0; bubble < kAuthoritySlotCount; ++bubble) {
            if (authority.holders[bubble] == generation) {
                authority.held[bubble] = false;
                authority.holders[bubble] = 0;
                // The link that takes the bubble over next resumes from the leaver's state.
                authority.releasedBy[bubble] = generation;
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/**
 * Releases only the reported bubble without changing its last grant token.
 * @param sessionId Joined activity session.
 * @param bubble Bubble named by the accepted abdication.
 * @param mask Optional released entities to merge into the pending claim.
 */
void record_abdication(std::uint64_t sessionId,
                       std::uint8_t bubble,
                       const EntitySlotMask* mask) noexcept {
    if (sessionId == kAbsentSessionId || bubble >= kAuthoritySlotCount) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        AuthorityState& authority = state.sessions[target].bubbleAuthority;
        // An abdication of a bubble nobody holds keeps the link that last released it.
        if (authority.holders[bubble] != 0) {
            authority.releasedBy[bubble] = authority.holders[bubble];
        }
        authority.held[bubble] = false;
        authority.holders[bubble] = 0;
        if (mask != nullptr) {
            auto& released = authority.releasedEntities[bubble];
            for (std::size_t index = 0; index < released.size(); ++index) {
                released[index] |= (*mask)[index];
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/**
 * A claim snapshots the complete pending mask without consuming it.
 * @param sessionId Joined activity session.
 * @param bubble Bubble whose pending entities are requested.
 * @param output Receives the pending mask, cleared on failure.
 * @return True when the mask contains any released entities.
 */
bool snapshot_released_entities(std::uint64_t sessionId,
                                std::uint8_t bubble,
                                EntitySlotMask& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId || bubble >= kAuthoritySlotCount) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        output = state.sessions[target].bubbleAuthority.releasedEntities[bubble];
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    for (const auto value : output) {
        if (value != std::byte{}) {
            return true;
        }
    }
    return false;
}

/**
 * A delivered claim preserves unrelated entities released after its snapshot.
 * @param sessionId Joined activity session.
 * @param bubble Bubble receiving the claim.
 * @param mask Exact delivered mask to subtract from pending entities.
 */
void record_claim(std::uint64_t sessionId,
                  std::uint8_t bubble,
                  const EntitySlotMask& mask) noexcept {
    if (sessionId == kAbsentSessionId || bubble >= kAuthoritySlotCount) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        auto& released = state.sessions[target].bubbleAuthority.releasedEntities[bubble];
        for (std::size_t index = 0; index < released.size(); ++index) {
            released[index] &= ~mask[index];
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/**
 * A committed purge removes only its delivered bits across this session's bubbles.
 * @param sessionId Joined activity session.
 * @param mask Exact delivered purge mask.
 */
void record_purge(std::uint64_t sessionId, const EntitySlotMask& mask) noexcept {
    if (sessionId == kAbsentSessionId) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        for (auto& released : state.sessions[target].bubbleAuthority.releasedEntities) {
            for (std::size_t index = 0; index < released.size(); ++index) {
                released[index] &= ~mask[index];
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Drops every grant recorded for one session, so the next roster push grants again. */
void clear_grants(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target != kInvalidSessionSlot) {
        // A join resets the roster container, not the client's token mirror, so issued stays.
        AuthorityState& authority = state.sessions[target].bubbleAuthority;
        authority.grantTokens = {};
        authority.held = {};
        authority.holders = {};
        authority.releasedBy = {};
        authority.releasedEntities = {};
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity::bubble_authority
