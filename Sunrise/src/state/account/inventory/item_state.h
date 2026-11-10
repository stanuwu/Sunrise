#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::account::inventory {

/** The native item record ends in 8 signed per-item values. */
inline constexpr std::size_t kItemObjectiveLaneCount = 8;
/**
 * Lane holding a bounty's deadline in Unix seconds; the client shows the deadline minus its
 * clock. No package field names this lane; the client's timer is the evidence for it.
 */
inline constexpr std::size_t kItemExpiryLane = 0;
/**
 * Per-instance values an item record replicates: a bounty's deadline, and the objective progress
 * its definition keeps per item.
 */
using ItemObjectives = std::array<std::int32_t, kItemObjectiveLaneCount>;

/** Native accumulated state bit that prevents item destruction. */
inline constexpr std::uint32_t kLockedItemFlag = 0x1U;
/** Native accumulated state bit that marks an item as tracked or favorite. */
inline constexpr std::uint32_t kTrackedItemFlag = 0x2U;
/** Native accumulated state bit that marks a completed masterwork. */
inline constexpr std::uint32_t kMasterworkItemFlag = 0x4U;
/** All accumulated item-state bits supported by the target build. */
inline constexpr std::uint32_t kSupportedItemStateMask =
    kLockedItemFlag | kTrackedItemFlag | kMasterworkItemFlag;

/**
 * @param flags Accumulated item-state value to check.
 * @return True when flags contains only item-state bits supported by the target build.
 */
[[nodiscard]] constexpr bool valid_item_state(std::uint32_t flags) noexcept {
    return (flags & ~kSupportedItemStateMask) == 0;
}

} // namespace sunrise::state::account::inventory
