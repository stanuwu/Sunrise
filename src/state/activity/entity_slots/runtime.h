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
    std::uint64_t sessionId{};
    /** Join carries the client key, which never changes for this session. */
    std::uint64_t memberKey{};
    /** A second copy, so a changed join key is caught before commit. */
    std::uint64_t expectedMemberKey{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedRecordRevision{};
    std::size_t requestedCount{};
    std::size_t targetSlot{kInvalidSessionSlot};
    MutationKind kind{};
    bool prepared{};
};

/**
 * Prepares a join. Picks only low-index slots the session does not already hold.
 * @param sessionId Existing nonzero activity session id.
 * @param memberKey Client member key from the same join request.
 * @param grantCount Slots requested, 1 to 8192 inclusive.
 * @param mutation Cleared, then receives the picked mask and revision data.
 * @return True when the session can commit the whole join.
 */
[[nodiscard]] bool prepare_join(std::uint64_t sessionId,
                                std::uint64_t memberKey,
                                std::size_t grantCount,
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

} // namespace sunrise::state::activity::entity_slots
