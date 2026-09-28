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
/**
 * Season Pass perks share this 5-slot character bucket with quest roots. A reward item here that
 * carries an acquisition flag is granted by setting that flag rather than as an inventory row.
 */
inline constexpr std::uint8_t kPerkBucketId = 37;
/** Leaving out the unavailable id leaves at most 255 unique bucket records. */
inline constexpr std::size_t kDescriptorCapacity = kUnavailableBucketId;
/** Signed -1 marks a bucket that has no equipment slot. */
inline constexpr std::int8_t kUnavailableEquipmentSlot = -1;
/** The packed descriptor holds routing, an optional equipment slot, and one reserved byte. */
inline constexpr std::size_t kDescriptorByteSize = 8;

/** Packed routing record for one checked runtime inventory bucket. */
struct Descriptor {
    std::uint8_t bucketId{};
    ArraySelector arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
    std::int8_t equipmentSlot{kUnavailableEquipmentSlot};
    std::uint8_t reserved{};
};

static_assert(sizeof(Descriptor) == kDescriptorByteSize);
static_assert(std::is_trivially_copyable_v<Descriptor>);

} // namespace sunrise::state::build_data::inventory::buckets
