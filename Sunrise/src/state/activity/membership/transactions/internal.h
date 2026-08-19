#pragma once

#include <cstddef>
#include <cstdint>

#include "../activity_membership_query.h"

namespace sunrise::state::activity::membership::transactions {

/** Each half of the packed refresh guard is 32 bits. */
inline constexpr std::uint8_t kRefreshGuardHalfWidth = 32;

/**
 * Packs a refresh request into a guard for its deferred transaction.
 * @param revision Membership revision the client saw.
 * @param bubbleIndex Logical bubble the client reported.
 * @return Guard value, recomputed before commit.
 */
inline std::uint64_t refresh_guard(std::uint32_t revision, std::int32_t bubbleIndex) noexcept {
    return (static_cast<std::uint64_t>(revision) << kRefreshGuardHalfWidth)
           | static_cast<std::uint32_t>(bubbleIndex);
}

/** @return True when both identities hold the same values. */
inline bool equal(const Identity& first, const Identity& second) noexcept {
    return first.memberKey == second.memberKey && first.smallOpaque == second.smallOpaque
           && first.signedOpaque == second.signedOpaque && first.joinIdentity == second.joinIdentity
           && first.accountSoid == second.accountSoid && first.opaqueSoid == second.opaqueSoid
           && first.secondaryOpaque == second.secondaryOpaque;
}

/** @return True when both stored spawn states hold the same values. */
inline bool equal(const SpawnState& first, const SpawnState& second) noexcept {
    return first.state == second.state && first.opaqueByte == second.opaqueByte
           && first.opaqueValue == second.opaqueValue;
}

/** @return True when both stored teleport states hold the same values. */
inline bool equal(const TeleportState& first, const TeleportState& second) noexcept {
    return first.state == second.state && first.token == second.token
           && first.sliceSetIndex == second.sliceSetIndex
           && first.sliceSetHash == second.sliceSetHash;
}

/** @return True when both reported regions hold the same index and hash. */
inline bool equal(const RegionState& first, const RegionState& second) noexcept {
    return first.index == second.index && first.hash == second.hash;
}

/** @return True when both sparse authoritative updates match field by field. */
inline bool equal(const AuthoritativeUpdate& first, const AuthoritativeUpdate& second) noexcept {
    return equal(first.spawn, second.spawn) && equal(first.teleport, second.teleport)
           && equal(first.region, second.region) && first.transitionToken == second.transitionToken
           && first.hasTransitionToken == second.hasTransitionToken
           && first.hasSpawn == second.hasSpawn && first.hasTeleport == second.hasTeleport
           && first.hasRegion == second.hasRegion;
}

/** @return True when both full refresh snapshots hold the same values. */
inline bool equal(const Snapshot& first, const Snapshot& second) noexcept {
    return equal(first.identity, second.identity) && equal(first.spawn, second.spawn)
           && equal(first.teleport, second.teleport) && first.revision == second.revision
           && first.epoch == second.epoch && first.transitionToken == second.transitionToken;
}

/**
 * Applies only the present authoritative fields to a copy of the membership value.
 * @param state Current membership value, left unchanged.
 * @param update Sparse fields to apply to the copy.
 * @return Merged membership value, with revisions and identity unchanged.
 */
inline MembershipState merge(const MembershipState& state,
                             const AuthoritativeUpdate& update) noexcept {
    MembershipState merged = state;
    if (update.hasTransitionToken) {
        merged.transitionToken = update.transitionToken;
        merged.hasTransitionToken = true;
    }
    if (update.hasSpawn) {
        merged.spawn = update.spawn;
    }
    if (update.hasTeleport) {
        merged.teleport = update.teleport;
    }
    // A negative region is the unset value the client sends on its way out, not a position.
    // Keeping it would drop the host's idea of the player back to the destination's own slice set.
    if (update.hasRegion && update.region.index >= kAbsentRegionIndex + 1) {
        merged.region = update.region;
    }
    return merged;
}

/**
 * Tests whether one delta moves the player to a different region.
 * The region is not a published membership field, so a move must not advance the revision. It
 * only decides whether the roster goes out at once or on the next tick.
 * @param state Current membership State.
 * @param merged Result of merging the delta into it.
 * @return True when the merge changed the reported region.
 */
inline bool moves_region(const MembershipState& state, const MembershipState& merged) noexcept {
    // The index alone names the bubble. A hash-only delta is the same position renamed, and
    // pushing the roster for it rebuilds every object the roster owns for nothing.
    return state.region.index != merged.region.index;
}

/**
 * Tests whether one delta moves the client's transition token.
 * @param state Current membership State.
 * @param merged Result of merging the delta into it.
 * @return True when the merge changed the token or set the first one.
 */
inline bool moves_transition_token(const MembershipState& state,
                                   const MembershipState& merged) noexcept {
    return state.transitionToken != merged.transitionToken
           || state.hasTransitionToken != merged.hasTransitionToken;
}

/** @return True when both State values would send the same authoritative fields. */
inline bool equal_authoritative(const MembershipState& first,
                                const MembershipState& second) noexcept {
    return first.transitionToken == second.transitionToken
           && first.hasTransitionToken == second.hasTransitionToken
           && equal(first.spawn, second.spawn) && equal(first.teleport, second.teleport);
}

/**
 * Matches a client identity against the key its own join request committed.
 * The key picks the member slot the client uses. Every other field is the client's to choose,
 * and fixing one refuses the identity, so no membership is ever published.
 * @param identity Candidate identity.
 * @param memberKey Key committed by this session's join request.
 * @return True when the candidate names this session's member slot.
 */
inline bool valid_identity(const Identity& identity, std::uint64_t memberKey) noexcept {
    return identity.memberKey == memberKey;
}

/**
 * Builds the exact refresh view for a candidate identity and revision.
 * @param state Current session membership State.
 * @param identity Candidate identity for the next snapshot.
 * @param revision Candidate membership revision, nonzero.
 * @return Full snapshot, built under the caller's lock.
 */
inline Snapshot make_snapshot(const MembershipState& state,
                              const Identity& identity,
                              std::uint32_t revision) noexcept {
    Snapshot snapshot{};
    snapshot.identity = identity;
    snapshot.spawn = state.spawn;
    snapshot.teleport = state.teleport;
    snapshot.revision = revision;
    snapshot.epoch = kStableEpoch;
    snapshot.transitionToken =
        state.hasTransitionToken ? state.transitionToken : kInitialTransitionToken;
    return snapshot;
}

/**
 * Captures one joined session and its shared transaction guards.
 * @param state Activity State, guarded by the root lock.
 * @param primarySoid Account identity, guarded by the same root lock.
 * @param sessionId Existing joined activity session id.
 * @param mutation Cleared, then receives the shared revision guards.
 * @return Matching joined record, or null when the session is absent or has not joined.
 */
[[nodiscard]] const SessionRecord* prepare_base(const ActivityState& state,
                                                std::uint64_t primarySoid,
                                                std::uint64_t sessionId,
                                                PendingMutation& mutation) noexcept;

/**
 * Advances root and record revisions after one stored membership change.
 * @param state Activity State held under the root write lock.
 * @param record Changed session record.
 */
void publish_change(ActivityState& state, SessionRecord& record) noexcept;

/**
 * Merges and applies one sparse authoritative operation.
 * @param state Activity State held under the root write lock.
 * @param record Target joined session record.
 * @param prepared Authoritative plan, consumed here.
 * @return True when the merge commits or leaves State unchanged.
 */
[[nodiscard]] bool commit_authoritative(ActivityState& state,
                                        SessionRecord& record,
                                        const PendingMutation& prepared) noexcept;

} // namespace sunrise::state::activity::membership::transactions
