#pragma once

#include <cstddef>

namespace sunrise::core::ui::memory {

/** 8 MiB caps all Dear ImGui context, font, widget, and draw storage. */
inline constexpr std::size_t kArenaCapacityBytes = 8'388'608;

/** Copied allocator counters. The arena storage itself is not exposed. */
struct Stats {
    bool installed{};
    std::size_t capacityBytes{kArenaCapacityBytes};
    std::size_t outstandingAllocations{};
    std::size_t outstandingBytes{};
    std::size_t highWaterBytes{};
    std::size_t largestFreeBytes{};
};

/**
 * Installs the fixed allocator before any Dear ImGui context exists.
 * @return True when the allocator is installed or was already installed.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Restores the earlier Dear ImGui allocator once every owned allocation is freed.
 * @return False while an allocation is still live; the allocator stays active.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return One copy of the allocator counters, read under the lock. */
[[nodiscard]] Stats snapshot() noexcept;

} // namespace sunrise::core::ui::memory
