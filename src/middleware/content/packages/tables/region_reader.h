#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "scenario_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Activity msg 12 publishes this many region records. */
inline constexpr std::size_t kRegionRecordCount = 64;
/** 64 bubbles spaced by the slice-set factor fill this region space. */
inline constexpr std::uint32_t kRegionIndexBound = 512;
/** The longest name is a 3-letter prefix and two 3-digit numbers around a dot. */
inline constexpr std::size_t kRegionNameCapacity = 16;

/** One formatted region name with its length. */
struct RegionName {
    std::array<char, kRegionNameCapacity> chars{};
    std::size_t length{};
};

/**
 * Converts a bubble ordinal to its region index.
 * @param bubbleIndex Bubble ordinal inside its scenario.
 * @return The region index the client uses for that bubble.
 */
[[nodiscard]] constexpr std::uint32_t region_index(std::uint32_t bubbleIndex) noexcept {
    return bubbleIndex * kSliceSetIndexFactor;
}

/**
 * Formats the region name the client publishes for one region index.
 * @param regionIndex Region index, below kRegionIndexBound.
 * @param publicBubble Picks the public prefix over the private one.
 * @param output Cleared before the checks and filled only on success.
 * @return True when the whole name and its null fit fixed storage.
 */
[[nodiscard]] bool
region_name(std::uint32_t regionIndex, bool publicBubble, RegionName& output) noexcept;

// The spacing is what makes 64 bubbles fill the region space exactly.
static_assert(region_index(0) == 0);
static_assert(region_index(1) == 8);
static_assert(region_index(13) == 104);
static_assert(region_index(63) == 504);
static_assert(region_index(64) == kRegionIndexBound);

} // namespace sunrise::middleware::content::packages::tables
