#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace sunrise::state::build_data::inventory::buckets {

/** Native inventory arrays a generated bucket descriptor can pick. */
enum class ArraySelector : std::uint8_t {
    /** Character-owned items use the fixed 350-slot character array. */
    character = 0,
    /** Account-owned items use the fixed 701-slot profile array. */
    profile = 1,
    /** Small account records use the fixed 6-slot profile array. */
    smallProfile = 2,
};

/** The character inventory ABI reserves 350 item slots. */
inline constexpr std::uint32_t kCharacterSlotCapacity = 350;
/** The main profile inventory ABI reserves 701 item slots. */
inline constexpr std::uint32_t kProfileSlotCapacity = 701;
/** The small profile inventory ABI reserves 6 item slots. */
inline constexpr std::uint32_t kSmallProfileSlotCapacity = 6;
/** An all-one bucket id means no runtime bucket is available. */
inline constexpr std::uint8_t kUnavailableBucketId = (std::numeric_limits<std::uint8_t>::max)();
/** Leaving out the unavailable id leaves at most 255 unique bucket records. */
inline constexpr std::size_t kDescriptorCapacity = kUnavailableBucketId;
/** The packed cache record holds 2 bytes and 2 16-bit slot fields. */
inline constexpr std::size_t kDescriptorByteSize = 6;

/** Packed routing record for one checked runtime inventory bucket. */
struct Descriptor {
    std::uint8_t bucketId{};
    ArraySelector arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
};

static_assert(sizeof(Descriptor) == kDescriptorByteSize);
static_assert(std::is_trivially_copyable_v<Descriptor>);

} // namespace sunrise::state::build_data::inventory::buckets
