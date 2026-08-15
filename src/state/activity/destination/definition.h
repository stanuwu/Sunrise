#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::destination {

/** The activity-selection schema carries one fixed 40-byte package name. */
inline constexpr std::size_t kPackageNameCapacity = 40;
/** 4 biased bits decode reasons from -1 to 14. */
inline constexpr std::int8_t kMinimumReason = -1;
/** 4 biased wire bits cannot hold a logical reason above 14. */
inline constexpr std::int8_t kMaximumReason = 14;
/** -1 means there is no previous activity index. */
inline constexpr std::int16_t kAbsentActivityIndex = -1;
/** 12 biased wire bits cannot hold a logical activity index above 4094. */
inline constexpr std::int16_t kMaximumActivityIndex = 4'094;
/** -1 is the logical value of an absent biased element index. */
inline constexpr std::int16_t kAbsentElementIndex = -1;
/** 9 biased wire bits cannot hold a logical element index above 510. */
inline constexpr std::int16_t kMaximumElementIndex = 510;
/** The client uses this hash to state that its selection names no spawn set. */
inline constexpr std::uint32_t kAbsentSpawnSetHash = 0x811C9DC5U;
/**
 * Storage for the selection descriptor's exact bits, which one outbound message replays verbatim.
 * Every dynamic count at its widest makes the descriptor 1,814 bits. The common form is 372.
 */
inline constexpr std::size_t kDescriptorCapacity = 232;

/** Safe scalar destination data kept from one activity selection. */
struct DestinationSelection final {
    /** Lowercase package bytes without an owning string or source descriptor. */
    std::array<std::int8_t, kPackageNameCapacity> packageName{};
    /** Package-name bytes in use. The rest of the fixed storage must stay zero. */
    std::uint8_t packageNameLength{};
    /** Logical selection reason after removing the wire bias. */
    std::int8_t reason{kMinimumReason};
    /** Logical source activity, or -1 when there is no previous activity. */
    std::int16_t previousActivityIndex{kAbsentActivityIndex};
    /** Destination definition index, or -1 when the destination was forced. */
    std::int16_t activityIndex{};
    /** Optional logical element index after removing the wire bias. */
    std::int16_t elementIndex{kAbsentElementIndex};
    /** Optional arrival-bubble name hash copied as one unsigned scalar. */
    std::uint32_t arrivalBubbleHash{};
    /** Optional spawn-set name hash copied as one unsigned scalar. */
    std::uint32_t spawnSetHash{};
    /** True only when elementIndex came from a present descriptor field. */
    bool hasElementIndex{};
    /** True only when arrivalBubbleHash came from a present descriptor field. */
    bool hasArrivalBubbleHash{};
    /** True only when spawnSetHash came from a present descriptor field. */
    bool hasSpawnSetHash{};
    /**
     * Authored arrival bubble for this destination, applied over every derived source.
     * A few destinations bind their arrival when the map loads rather than declaring it in the
     * packages, so no walk can derive those and they are authored per name instead.
     */
    std::uint8_t arrivalBubbleOverride{};
    /** True only when an authored override named this destination. */
    bool hasArrivalBubbleOverride{};
    /**
     * Slice-set index for this destination, applied over every derived source.
     * A bubble can declare several slice sets, so naming the bubble alone cannot reach the ones
     * past its first. Only a forced destination sets this.
     */
    std::uint16_t sliceSetOverride{};
    /** True only when a forced destination named a slice set. */
    bool hasSliceSetOverride{};
    /** Authored spawn-set hash for this destination, applied over every derived source. */
    std::uint32_t spawnSetOverride{};
    /** True only when an authored override named a spawn set for this destination. */
    bool hasSpawnSetOverride{};
    /**
     * The selection descriptor's own bits, left-aligned, exactly as the client encoded them.
     * The host echoes this descriptor back, and it carries fields with no known name. Re-encoding
     * from the scalars above would drop those, so the bits are replayed instead.
     */
    std::array<std::byte, kDescriptorCapacity> descriptorBits{};
    /** Meaningful bits in descriptorBits, or zero when no descriptor was captured. */
    std::uint16_t descriptorBitLength{};
    /** First name byte's bit offset inside descriptorBits. A forced name is written there. */
    std::uint16_t descriptorNameBit{};
    /** True only when the captured descriptor carried the fixed package-name field. */
    bool hasDescriptorName{};
};

/**
 * Tests whether a client selection gives a usable spawn-set hash.
 * @param present True when the descriptor carried the optional field.
 * @param value Hash carried by that field.
 * @return True when the present field names a nonempty spawn set.
 */
[[nodiscard]] constexpr bool usable_spawn_set_hash(bool present, std::uint32_t value) noexcept {
    return present && value != 0 && value != kAbsentSpawnSetHash;
}

/**
 * Finds one destination's spawn-set hash without changing its kept descriptor fields.
 * @param selection Committed destination with raw wire fields and any authored override.
 * @param fallback Authored fallback hash.
 * @return The authored override, the usable client hash, or the fallback, in that order.
 */
[[nodiscard]] constexpr std::uint32_t resolve_spawn_set_hash(const DestinationSelection& selection,
                                                             std::uint32_t fallback) noexcept {
    if (selection.hasSpawnSetOverride) {
        return selection.spawnSetOverride;
    }
    return usable_spawn_set_hash(selection.hasSpawnSetHash, selection.spawnSetHash)
               ? selection.spawnSetHash
               : fallback;
}

} // namespace sunrise::state::activity::destination
