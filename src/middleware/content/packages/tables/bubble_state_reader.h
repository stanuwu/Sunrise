#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "scenario_reader.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Activity msg 1 carries one byte per bubble, and no installed scenario declares more than this.
 * The region space is 512 wide with bubbles spaced by the slice-set factor, so this is the count
 * that fits it exactly.
 */
inline constexpr std::size_t kBubbleStateCapacity = 64;
/** The byte a bubble carries when its first slice-set state is enabled. */
inline constexpr std::uint8_t kBubbleEnabledByte = 0x80;
/** The byte every other bubble carries, including one with no readable state array. */
inline constexpr std::uint8_t kBubbleDisabledByte = 0x7F;
/** A bubble's state count is stored in one byte, so a wider array is reported at this ceiling. */
inline constexpr std::uint64_t kBubbleStateCountCeiling = 255;
/** Packages one scenario's slice-set entries name. The widest installed destination names 3. */
inline constexpr std::size_t kScenarioPackageCapacity = 8;

static_assert(kBubbleStateCapacity * kSliceSetIndexFactor == 512);

/** The per-bubble state array activity msg 1 publishes. */
struct BubbleStates {
    std::array<std::uint8_t, kBubbleStateCapacity> bytes{};
    /**
     * Each bubble's own name hash, in the same order.
     * The client names its arrival bubble by hash, so this is what turns that hash into the index
     * whose slice set the host publishes.
     */
    std::array<std::uint32_t, kBubbleStateCapacity> hashes{};
    /**
     * Slice-set states each bubble declares, in the same order.
     * A bubble owns a contiguous run of slice sets starting at its region index, so this count is
     * what says how many of them exist.
     */
    std::array<std::uint8_t, kBubbleStateCapacity> stateCounts{};
    /**
     * Each bubble's map-global index, in the same order.
     * Container bubble masks are keyed by this index, so it is what turns a bubble ordinal into
     * the spawn sets that bubble offers. Absent bubbles carry `kAbsentMapBubbleIndex`.
     */
    std::array<std::uint16_t, kBubbleStateCapacity> mapIndices{};
    /**
     * Distinct packages the scenario's slice-set entries name, plus its own.
     * A destination loads these, so a spawn set from an activity package outside this list is one
     * this destination does not stream.
     */
    std::array<std::uint16_t, kScenarioPackageCapacity> packages{};
    std::size_t packageCount{};
    std::size_t count{};
    /** Set when the scenario declares more bubbles than the wire array holds. */
    bool truncated{};
};

/**
 * Builds one scenario's per-bubble state array.
 * Every byte comes from the scenario blob alone, so this needs no nested tag reads and none of
 * the entry, registry or object chain.
 * @param scenario Whole scenario bytes.
 * @param output Receives the array and its length.
 * @return True when the scenario declares a bubble array.
 */
[[nodiscard]] bool bubble_states(std::span<const std::byte> scenario,
                                 BubbleStates& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
