#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::content::packages::tables {

/** Definition tags sit in this closed range. */
inline constexpr std::uint32_t kTagLowerBound = 0x80800000;
/** Values above this are not definition tags. */
inline constexpr std::uint32_t kTagUpperBound = 0x82000000;
/** A tag holds its entry index in the low bits and its package id above them. */
inline constexpr std::uint32_t kTagPackageShift = 13;
/** The package id no tag names, reported for a value outside the tag range. */
inline constexpr std::uint16_t kAbsentPackageId = 0xFFFFU;

/**
 * Reads the package id one tag names.
 * It is the same number the package file carries in its name.
 * @param tag Definition tag.
 * @return The package id, or `kAbsentPackageId` when the value is not a tag.
 */
[[nodiscard]] constexpr std::uint16_t package_of(std::uint32_t tag) noexcept {
    if (tag < kTagLowerBound || tag >= kTagUpperBound) {
        return kAbsentPackageId;
    }
    return static_cast<std::uint16_t>((tag - kTagLowerBound) >> kTagPackageShift);
}

/** Element class of the item index table inside the investment container. */
inline constexpr std::uint32_t kItemIndexTableClass = 0x80807BE8U;
/** Element class of an item's ordinary socket array. */
inline constexpr std::uint32_t kOrdinarySocketClass = 0x808077C4U;
/** One ordinary socket entry is 80 bytes. */
inline constexpr std::size_t kSocketEntryStride = 80;
/** An item definition holds its socket block self-relative at this offset. */
inline constexpr std::size_t kSocketBlockOffset = 104;
/** An item definition holds its inventory bucket id at this offset. */
inline constexpr std::size_t kBucketIdOffset = 184;
/** An item definition holds its art block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kArtBlockOffset = 136;
/** Element class of an item's art row array. */
inline constexpr std::uint32_t kArtRowClass = 0x808077B5U;
/** Element class of an item's material override array. */
inline constexpr std::uint32_t kMaterialOverrideClass = 0x808077B3U;
/** Element class of an item's sandbox perk array. */
inline constexpr std::uint32_t kSandboxPerkClass = 0x808077BCU;
/** An item definition holds its stat block self-relative at this offset, zero when absent. */
inline constexpr std::size_t kStatBlockOffset = 112;
/** The investment root holds the constants blob at this slot. */
inline constexpr std::size_t kInvestmentConstantsSlot = 11;
/** The investment root holds the progression definition table at this slot. */
inline constexpr std::size_t kProgressionTableSlot = 68;
/** Element class of the progression definition table. */
inline constexpr std::uint32_t kProgressionTableClass = 0x80807CDDU;
/** One progression definition is 88 inline bytes. */
inline constexpr std::size_t kProgressionRowStride = 88;
/** A progression definition names the object array holding its record here. */
inline constexpr std::size_t kProgressionScopeOffset = 4;

/** One array found inside a definition blob. */
struct Array {
    std::uint64_t count{};
    std::size_t dataOffset{};
    std::uint32_t elementClass{};
};

/** One row of a definition index table. */
struct IndexRow {
    std::uint32_t definitionHash{};
    std::uint32_t targetTag{};
};

/** Visitor called once per index-table row. */
using RowVisitor = bool (*)(void* context, std::uint32_t index, const IndexRow& row) noexcept;

/**
 * Reads the array descriptor at one known offset.
 * @param blob Whole definition bytes.
 * @param descriptorOffset Offset of the count and self-relative offset pair.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a valid array header.
 */
[[nodiscard]] bool find_array_at(std::span<const std::byte> blob,
                                 std::size_t descriptorOffset,
                                 Array& output) noexcept;

/**
 * Finds the first array whose header names one element class.
 * @param blob Whole definition bytes.
 * @param elementClass Element class the header must carry.
 * @param output Cleared before the search and filled only on a match.
 * @return True when one descriptor points at a header of that class.
 */
[[nodiscard]] bool
find_array(std::span<const std::byte> blob, std::uint32_t elementClass, Array& output) noexcept;

/**
 * Reads every row of one index table in order and hands each to a visitor.
 * @param blob Whole definition bytes.
 * @param array Index-table array that was found.
 * @param visitor Called once per row. Returning false ends the walk.
 * @param context Opaque pointer handed back to the visitor.
 * @return True when every row read back and the visitor accepted all of them.
 */
[[nodiscard]] bool visit_index_rows(std::span<const std::byte> blob,
                                    const Array& array,
                                    RowVisitor visitor,
                                    void* context) noexcept;

/**
 * Walks the child tags of one container blob.
 * @param blob Whole container bytes.
 * @param index Child ordinal.
 * @param tag Receives the child tag.
 * @return True when the ordinal is inside the container.
 */
[[nodiscard]] bool
child_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept;

/**
 * Reads one investment-root slot.
 * @param blob Whole root bytes.
 * @param index Slot ordinal.
 * @param tag Receives the slot tag.
 * @return True when the slot is inside the root.
 */
[[nodiscard]] bool
slot_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept;

/** The investment root holds the item index table at this slot. */
inline constexpr std::size_t kItemTableSlot = 48;
/** The investment root holds the socket entry list table at this slot. */
inline constexpr std::size_t kSocketEntryListTableSlot = 97;
/** The globals container names the investment root as its first child. */
inline constexpr std::size_t kInvestmentRootChild = 0;
/** The investment root holds the inventory bucket table at this slot. */
inline constexpr std::size_t kBucketTableSlot = 17;
/** Element class of the socket entry list table. */
inline constexpr std::uint32_t kSocketEntryListTableClass = 0x80807A7EU;
/** A socket entry list definition holds its entry array descriptor here. */
inline constexpr std::size_t kSocketEntryArrayDescriptor = 16;
/** One socket entry is 64 bytes. */
inline constexpr std::size_t kSocketEntrySize = 64;
/** A socket entry holds its plug source hash here. */
inline constexpr std::size_t kSocketEntryPlugSource = 8;
/** Bucket group this socket entry competes in. `0xFF` marks an entry with no group. */
inline constexpr std::size_t kSocketEntryGroup = 12;
/** Entry kind. Kind 34 is the super lane, which is active despite carrying no plug source. */
inline constexpr std::size_t kSocketEntryKind = 13;
/** The absent plug source. */
inline constexpr std::uint32_t kNoPlugSource = 0x811C9DC5U;
/** The bucket table declares its descriptor count here. */
inline constexpr std::size_t kBucketCountOffset = 140;
/** Bucket descriptors follow the count. */
inline constexpr std::size_t kBucketFirstDescriptor = 144;
/** One bucket descriptor is 36 bytes. */
inline constexpr std::size_t kBucketDescriptorSize = 36;
/** Bucket descriptor field offsets. */
inline constexpr std::size_t kBucketFirstSlotOffset = 4;
inline constexpr std::size_t kBucketSlotCountOffset = 8;
inline constexpr std::size_t kBucketArraySelectorOffset = 24;

/** Every definition table holds its array descriptor at this fixed offset. */
inline constexpr std::size_t kTableArrayDescriptor = 8;

/** @param blob Whole container bytes. @return Its child count. */
[[nodiscard]] std::size_t child_count(std::span<const std::byte> blob) noexcept;

/**
 * Reads one row of an index table that was found.
 * @param blob Whole definition bytes owning the array.
 * @param array Array with a 24-byte stride.
 * @param index Row ordinal.
 * @param row Receives the row fields.
 * @return True when the row is inside the blob.
 */
[[nodiscard]] bool index_row(std::span<const std::byte> blob,
                             const Array& array,
                             std::uint64_t index,
                             IndexRow& row) noexcept;

} // namespace sunrise::middleware::content::packages::tables
