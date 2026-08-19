#pragma once

#include <cstddef>
#include <cstdint>

#include "../definition.h"

namespace sunrise::state::activity::membership {

/** Snapshot and revision guards for one deferred membership operation. */
struct PendingMutation final {
    Snapshot snapshot{};
    /** A second copy, so a changed identity plan is caught before commit. */
    Identity identityGuard{};
    /** Sparse authoritative input, kept whether or not a delivery snapshot exists. */
    AuthoritativeUpdate authoritativeInput{};
    /** A second copy, so a changed authoritative plan is caught before commit. */
    AuthoritativeUpdate authoritativeGuard{};
    std::uint64_t sessionId{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedRecordRevision{};
    std::uint64_t expectedPrimarySoid{};
    std::uint64_t refreshRequestGuard{};
    std::uint32_t requestedRevision{};
    std::uint32_t acknowledgement{};
    std::int32_t bubbleIndex{};
    std::size_t targetSlot{kInvalidSessionSlot};
    MutationKind kind{};
    bool hasSnapshot{};
    bool changesState{};
    /**
     * Set when the delta moves the player to a different region.
     * The region is not a published field, so this is separate from changesState. A move rebuilds
     * the citizen advertisement, advances the revision and sends the roster at once.
     */
    bool movesRegion{};
    /**
     * Set when the delta changes the client's transition token.
     * The token goes up once per load, so a change is the only start signal on the wire.
     */
    bool movesTransitionToken{};
    bool prepared{};
};

/**
 * Prepares one exact identity for a joined activity session.
 * @param sessionId Existing joined activity session id.
 * @param identity Copied into the candidate refresh snapshot.
 * @param mutation Cleared, then receives the snapshot and revision guards.
 * @return True when the identity is valid, including an unchanged duplicate.
 */
[[nodiscard]] bool prepare_identity(std::uint64_t sessionId,
                                    const Identity& identity,
                                    PendingMutation& mutation) noexcept;

/**
 * Prepares sparse host-state changes for one joined activity session.
 * @param sessionId Existing joined activity session id.
 * @param update Kept fields, each with its own presence flag.
 * @param mutation Cleared, then receives State guards and an optional new snapshot.
 * @return True for a joined session, including an unchanged no-op.
 */
[[nodiscard]] bool prepare_authoritative(std::uint64_t sessionId,
                                         const AuthoritativeUpdate& update,
                                         PendingMutation& mutation) noexcept;

/**
 * Captures the current membership snapshot without changing stored State.
 * @param sessionId Existing joined activity session id.
 * @param requestedRevision Membership revision the client last saw.
 * @param bubbleIndex Logical bubble the client reported, carried through unchecked.
 * @param mutation Cleared output; hasSnapshot is false before the first identity.
 * @return True when the session is joined.
 */
[[nodiscard]] bool prepare_refresh(std::uint64_t sessionId,
                                   std::uint32_t requestedRevision,
                                   std::int32_t bubbleIndex,
                                   PendingMutation& mutation) noexcept;

/**
 * Tests whether the client has applied the current membership revision.
 * Every membership push makes the client rebuild its region table and its player snapshots, so an
 * acknowledged snapshot is re-sent only when something changes it.
 * @param sessionId Joined activity session.
 * @return True when the published revision has been acknowledged.
 */
[[nodiscard]] bool acknowledged(std::uint64_t sessionId) noexcept;

/**
 * Prepares a membership-revision advance so an already-applied snapshot can be corrected.
 * The client applies one update per revision and drops every repeat, so a body published with a
 * stale citizen advertisement can only be replaced at a new revision.
 * @param sessionId Joined activity session.
 * @param mutation Cleared, then receives the next snapshot and exact State guards.
 * @return True when the next revision can be staged.
 */
[[nodiscard]] bool prepare_republish(std::uint64_t sessionId, PendingMutation& mutation) noexcept;

/**
 * Reads the region the client last reported it was in.
 * The client names its own position and it changes as the player crosses a bubble boundary, so it
 * beats the destination's own arrival slice set wherever the host must say where the player is.
 * @param sessionId Joined activity session.
 * @return The reported region index, or -1 when none has arrived.
 */
[[nodiscard]] std::int32_t reported_region(std::uint64_t sessionId) noexcept;

/**
 * Reads the newest session the client has reported a region on.
 * A link that joined a session it did not allocate never reports one. Session ids rise, so the
 * newest is the live one.
 * @param fallback Returned when no session has a reported region.
 * @return That session id, or the fallback.
 */
[[nodiscard]] std::uint64_t live_region_session(std::uint64_t fallback) noexcept;

/**
 * Reads the identity value message 12 publishes at member record `+16`.
 * The seed fills it with the join's member key. The client's own identity message replaces it.
 * @param sessionId Joined activity session.
 * @return The join identity, or zero before any identity is published.
 */
[[nodiscard]] std::uint64_t join_identity(std::uint64_t sessionId) noexcept;

/**
 * Prepares an acknowledgement mark for the current membership revision.
 * @param sessionId Existing joined activity session id.
 * @param revision Revision the client applied; stale and future values are valid no-ops.
 * @param mutation Cleared, then receives the revision guards.
 * @return True when the joined session and its current membership State are valid.
 */
[[nodiscard]] bool prepare_acknowledgement(std::uint64_t sessionId,
                                           std::uint32_t revision,
                                           PendingMutation& mutation) noexcept;

/**
 * Commits one identity, authoritative, refresh, republish, or acknowledgement operation.
 * @param mutation Prepared plan. Always cleared before this function returns.
 * @return True when the operation commits or needs no State change.
 */
[[nodiscard]] bool commit(PendingMutation& mutation) noexcept;

} // namespace sunrise::state::activity::membership
