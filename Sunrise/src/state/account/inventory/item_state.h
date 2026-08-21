#pragma once

#include <cstdint>

namespace sunrise::state::account::inventory {

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
