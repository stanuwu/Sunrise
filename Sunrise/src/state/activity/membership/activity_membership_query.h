#pragma once

#include <cstddef>
#include <cstdint>

#include "../definition.h"
#include "member_directory.h"

namespace sunrise::state::activity::entity_slots {
struct PendingMutation;
}

namespace sunrise::state::activity::membership {

/** Snapshot and revision guards for one deferred membership operation. */
struct PendingMutation final {
    Snapshot snapshot{};
    reservations::Roster peerSnapshot{};
    MemberDirectory memberDirectory{};
    /** A second copy, so a changed identity plan is caught before commit. */
    Identity identityGuard{};
    /** Sparse client-state input, kept whether or not a delivery snapshot exists. */
    AuthoritativeUpdate authoritativeInput{};
    /** A second copy, so a changed client-state plan is caught before commit. */
    AuthoritativeUpdate authoritativeGuard{};
    std::uint64_t sessionId{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedRecordRevision{};
    std::uint64_t expectedPrimarySoid{};
    std::uint64_t expectedMemberKey{};
    std::size_t memberRow{entity_slots::kMemberLeaseRowCount};
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

/** Exact membership after-image of a validated native join and its deferred lease transaction. */
[[nodiscard]] bool prepare_join_snapshot(const entity_slots::PendingMutation& join,
                                         PendingMutation& mutation) noexcept;

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

/** Current recipient's publishable revision, or zero for an absent/replaced binding. */
[[nodiscard]] std::uint32_t current_revision(const SessionBinding& binding) noexcept;

/**
 * Arms or clears the host-named region the client teleports to.
 * The client reads these fields out of the message-12 region-block tail and starts a type-7
 * slice-set transition to the named region, so this is the host's only mid-activity move.
 * @param sessionId Joined activity session to move.
 * @param sliceSetIndex Authored region to move to, or the absent sentinel to clear the arm.
 * @param spawnSetHash Spawn set filter after the move; the empty-name hash means `default`.
 * @return True when the session exists and the arm changed.
 */
[[nodiscard]] bool arm_host_teleport(std::uint64_t sessionId,
                                     std::int32_t sliceSetIndex,
                                     std::uint32_t spawnSetHash) noexcept;

/**
 * Arms one native hard wipe on an exact session generation. Arming the same request key again
 * is accepted; another key is refused while a wipe or a host teleport is active.
 * @param requestKey Script request that owns the wipe. @param region Region the party is in.
 * @param spawnSetHash Authored spawn set the party respawns at.
 */
[[nodiscard]] bool arm_hard_wipe(const SessionBinding& binding,
                                 std::uint64_t requestKey,
                                 std::int32_t region,
                                 std::uint32_t spawnSetHash) noexcept;

/** @return True while an armed wipe's spawn block still has to reach the client. */
[[nodiscard]] bool hard_wipe_needs_publish(std::uint64_t sessionId) noexcept;

/** @return True while the client has not yet mirrored an armed wipe's start block. */
[[nodiscard]] bool hard_wipe_start_unseen(std::uint64_t sessionId) noexcept;

/** Releases the wipe the request key armed, so the client's wait is answered. */
[[nodiscard]] bool release_hard_wipe(const SessionBinding& binding,
                                     std::uint64_t requestKey) noexcept;

/** @return The spawn set the armed wipe named for this region, or zero. */
[[nodiscard]] std::uint32_t checkpoint_spawn_hash(std::uint64_t sessionId,
                                                  std::int32_t region) noexcept;

/** Records the region the attached mission program declared as its initial state. */
void note_declared_initial_region(std::uint64_t sessionId, std::int32_t region) noexcept;

/** Records the spawn set the attached mission program declared with its initial state. */
void note_declared_spawn_set(std::uint64_t sessionId, std::uint32_t spawnSetHash) noexcept;

/** Holds or releases the mission program's spawn hold for one joined session. */
void set_program_spawn_hold(std::uint64_t sessionId, bool held) noexcept;

/** @return True while the mission program holds this session's spawn. */
[[nodiscard]] bool program_spawn_hold(std::uint64_t sessionId) noexcept;

/** @return The declared initial-state region, or -1 while no program declared one. */
[[nodiscard]] std::int32_t declared_initial_region(std::uint64_t sessionId) noexcept;

/** @return The spawn set the attached program declared, or zero while none is declared. */
[[nodiscard]] std::uint32_t declared_spawn_set(std::uint64_t sessionId) noexcept;

/** @return The current activity State revision. */
[[nodiscard]] std::uint64_t state_revision() noexcept;

/**
 * @param sessionId Joined activity session.
 * @return True while a host-named teleport is armed and unspent.
 */
[[nodiscard]] bool host_teleport_armed(std::uint64_t sessionId) noexcept;

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
 * Reads the pending region leg the client last reported.
 * This is the region it is loading or precaching, so it names a target before the client is there.
 * After a z-leg switch it names the region behind the player; after a completed transition, -1.
 * @param sessionId Joined activity session.
 * @return The pending region index, or -1.
 */
[[nodiscard]] std::int32_t reported_region(std::uint64_t sessionId) noexcept;

/**
 * Reads the region the client is in.
 * The current leg names the slice set the client holds. While it holds none, the pending leg
 * names where it is loading into, and that is where the host has to say the player is.
 * @param sessionId Joined activity session.
 * @return The current region, else the pending one, else -1.
 */
[[nodiscard]] std::int32_t player_region(std::uint64_t sessionId) noexcept;

/**
 * Reads the slice set from the client's newest D6 teleport state.
 * Unlike the reported region, this stays on the package slice selected for the transition while
 * the player moves between regions inside that slice.
 * @param sessionId Joined activity session.
 * @return The reported slice-set index, or -1 before a D6 teleport state arrives.
 */
[[nodiscard]] std::int32_t reported_slice_set(std::uint64_t sessionId) noexcept;

/** Where the client says it is: its two region legs and the bubble its refresh names current. */
struct ClientPlacement final {
    /** Pending region leg: the load or precache target, or the region behind a z-leg switch. */
    std::int32_t region{kAbsentRegionIndex};
    /** Current region leg: the region of the slice set the client holds, -1 while none. */
    std::int32_t currentRegion{kAbsentRegionIndex};
    std::int32_t bubble{kMinimumRefreshBubble};
    /** Membership revision the refresh that named the bubble had applied. */
    std::uint32_t bubbleRevision{kAbsentRevision};
};

/**
 * Reads the client's region legs and current bubble together.
 * @param sessionId Joined activity session.
 * @return All three reports, each at its absent value until it arrives.
 */
[[nodiscard]] ClientPlacement reported_placement(std::uint64_t sessionId) noexcept;

/**
 * Names the region the client has instantiated.
 * The current leg of the client's member record follows the slice-set manager. It moves only
 * when a slice-set switch has run, and reads -1 while no slice set is held.
 * @param placement The client's reports.
 * @return The current region, or -1 while the client holds none.
 */
[[nodiscard]] std::int32_t instantiated_region(const ClientPlacement& placement) noexcept;

/**
 * Reads the newest session the client has reported a region on.
 * A link that joined a session it did not allocate never reports one. Session ids rise, so the
 * newest is the live one.
 * @param fallback Returned when no session has a reported region.
 * @return That session id, or the fallback.
 */
[[nodiscard]] std::uint64_t live_region_session(std::uint64_t fallback) noexcept;

/**
 * Reads the member key the client joined this activity session under.
 * A peer that joins the gameplay group carries the same key as its join id.
 * @param sessionId Joined activity session.
 * @return The member key, or zero before any identity is published.
 */
[[nodiscard]] std::uint64_t member_key(std::uint64_t sessionId) noexcept;

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
 * Commits one identity, client-state, refresh, republish, or acknowledgement operation.
 * @param mutation Prepared plan. Always cleared before this function returns.
 * @return True when the operation commits or needs no State change.
 */
[[nodiscard]] bool commit(PendingMutation& mutation,
                          CommittedClientState* clientState = nullptr) noexcept;

} // namespace sunrise::state::activity::membership
