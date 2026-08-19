#pragma once

#include <cstddef>
#include <cstdint>

#include "../definition.h"

namespace sunrise::state::activity::entity_slots {

/** Mask and revision data for one deferred lease change. */
struct PendingMutation final {
    LeaseMask mask{};
    /** Kept only to recompute a release under the write lock. */
    LeaseMask returnedMask{};
    /** Server complement chosen by a join. Empty for every other kind. */
    LeaseMask serverMask{};
    std::uint64_t sessionId{};
    /** Join carries the client key, which never changes for this session. */
    std::uint64_t memberKey{};
    /** A second copy, so a changed join key is caught before commit. */
    std::uint64_t expectedMemberKey{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedRecordRevision{};
    std::size_t requestedCount{};
    /** Slots the join holds back for server-authored entities. */
    std::size_t serverReserveCount{};
    std::size_t targetSlot{kInvalidSessionSlot};
    MutationKind kind{};
    bool prepared{};
};

/**
 * Prepares a join and the server complement in one plan.
 * Both masks commit together. Deriving one after the commit would let a grant see the client
 * mask against an older reserve.
 * @param sessionId Existing nonzero activity session id.
 * @param memberKey Client member key from the same join request.
 * @param grantCount Slots requested, at least 1 and at most the slot count less the reserve.
 * @param serverReserveCount High slots held back for server-authored entities.
 * @param mutation Cleared, then receives both masks and the revision data.
 * @return True when the session can commit the whole join.
 */
[[nodiscard]] bool prepare_join(std::uint64_t sessionId,
                                std::uint64_t memberKey,
                                std::size_t grantCount,
                                std::size_t serverReserveCount,
                                PendingMutation& mutation) noexcept;

/**
 * Prepares up to the requested number of free low-index slots.
 * @param sessionId Existing joined activity session id.
 * @param requestedCount Must be positive. Above the slot count, all free slots are picked.
 * @param mutation Cleared, then receives the picked mask and revision data.
 * @return True for every valid joined request, including a zero-mask grant when full.
 */
[[nodiscard]] bool prepare_grant(std::uint64_t sessionId,
                                 std::size_t requestedCount,
                                 PendingMutation& mutation) noexcept;

/**
 * Prepares the overlap of returned and currently held slots.
 * @param sessionId Existing joined activity session id.
 * @param returned Full client-returned slot mask in logical byte order.
 * @param mutation Cleared, then receives the picked mask and revision data.
 * @return True for every valid joined session, including an empty overlap.
 */
[[nodiscard]] bool prepare_release(std::uint64_t sessionId,
                                   const LeaseMask& returned,
                                   PendingMutation& mutation) noexcept;

/**
 * Commits one join, grant, or release when its captured revisions still match.
 * @param mutation Prepared plan. Always cleared before this function returns.
 * @return True when it committed, or the picked mask needed no State change.
 */
[[nodiscard]] bool commit(PendingMutation& mutation) noexcept;

/**
 * Reports how many slots one session currently leases.
 * The client cannot create a simulation entity without a free index, and nothing else on this
 * path says how many it has, so a run that fails to create one can be read against the lease.
 * @param sessionId Activity session id.
 * @param held Receives the slots the client holds.
 * @param reserved Receives the slots the host held back for itself.
 * @return True when the session exists and has joined.
 */
[[nodiscard]] bool
lease_counts(std::uint64_t sessionId, std::size_t& held, std::size_t& reserved) noexcept;

/**
 * Copies both lease masks one session currently holds.
 * The physics replica context needs the bits, not the counts: an allocation picks one exact index
 * out of the server reserve, and State refuses a context whose two masks overlap.
 * @param sessionId Activity session id.
 * @param held Receives the slots the client leases.
 * @param reserved Receives the slots the host held back for itself.
 * @return True when the session exists and has joined.
 */
[[nodiscard]] bool
lease_masks(std::uint64_t sessionId, LeaseMask& held, LeaseMask& reserved) noexcept;

} // namespace sunrise::state::activity::entity_slots
