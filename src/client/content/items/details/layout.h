#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::content::items::details::layout {

/** The optional equipment block is self-relative from definition byte 16. */
inline constexpr std::size_t kEquipmentBlockRelativeOffset = 16;
/** The optional ordinary-socket block is self-relative from definition byte 104. */
inline constexpr std::size_t kOrdinarySocketBlockRelativeOffset = 104;
/** The optional socket-list block is self-relative from definition byte 128. */
inline constexpr std::size_t kSocketListBlockRelativeOffset = 128;
/** The native signed stack limit sits at definition byte 180. */
inline constexpr std::size_t kMaxStackSizeOffset = 180;
/** The native inventory bucket id sits at definition byte 184. */
inline constexpr std::size_t kBucketIdOffset = 184;
/** Definition byte 187 is zero for stackable rows and nonzero for instanced rows. */
inline constexpr std::size_t kInstancedDefinitionPredicateOffset = 187;
/** The equipment block stores its signed slot id at byte 24. */
inline constexpr std::size_t kEquipmentSlotOffset = 24;
/** The array header begins relative to the offset member at array-field byte 8. */
inline constexpr std::size_t kArrayHeaderRelativeOffset = 8;
/** The array marker sits in the 4 bytes right before the resolved header. */
inline constexpr std::size_t kArrayMarkerBackOffset = sizeof(std::uint32_t);
/** Loaded native arrays use this marker before their resolved header. */
inline constexpr std::uint32_t kArrayHeaderMarker = 0x80809FBDU;
/** Ordinary socket arrays declare this native element class. */
inline constexpr std::uint32_t kOrdinarySocketElementClass = 0x808077C4U;
/** Array entries start 16 bytes after the resolved header. */
inline constexpr std::size_t kArrayFirstEntryOffset = 16;
/** One ordinary socket entry is 80 native bytes. */
inline constexpr std::size_t kOrdinarySocketEntrySize = 80;
/** An ordinary socket entry stores its initial plug index at byte 2. */
inline constexpr std::size_t kInitialPlugIndexOffset = 2;

/** Self-relative array field at the start of the ordinary-socket block. */
struct RelativeArray {
    std::uint64_t count{};
    std::int64_t headerRelative{};
};

/** Fixed item-definition fields used by configured detail extraction. */
struct ItemDefinition {
    std::array<std::byte, kEquipmentBlockRelativeOffset> opaqueBeforeEquipmentBlock{};
    std::int64_t equipmentBlockRelative{};
    std::array<std::byte,
               kOrdinarySocketBlockRelativeOffset - kEquipmentBlockRelativeOffset
                   - sizeof(std::int64_t)>
        opaqueBeforeOrdinarySocketBlock{};
    std::int64_t ordinarySocketBlockRelative{};
    /** Stats are read from the package blob, never from loaded memory, so this stays opaque. */
    std::array<std::byte,
               kSocketListBlockRelativeOffset - kOrdinarySocketBlockRelativeOffset
                   - sizeof(std::int64_t)>
        opaqueBeforeSocketListBlock{};
    std::int64_t socketListBlockRelative{};
    std::array<std::byte,
               kMaxStackSizeOffset - kSocketListBlockRelativeOffset - sizeof(std::int64_t)>
        opaqueBeforeMaxStackSize{};
    std::int32_t maxStackSize{};
    std::uint8_t bucketId{};
    std::array<std::byte,
               kInstancedDefinitionPredicateOffset - kBucketIdOffset - sizeof(std::uint8_t)>
        opaqueBeforeInstancedDefinitionPredicate{};
    std::uint8_t instancedDefinitionPredicate{};
};

/** Fixed prefix of the optional equipment block. */
struct EquipmentBlock {
    std::array<std::byte, kEquipmentSlotOffset> opaqueBeforeSlot{};
    std::int8_t slot{};
};

/** One fixed native ordinary-socket entry. */
struct OrdinarySocketEntry {
    std::uint16_t socketTypeIndex{};
    std::uint16_t initialPlugIndex{};
    std::array<std::byte,
               kOrdinarySocketEntrySize - kInitialPlugIndexOffset - sizeof(std::uint16_t)>
        opaqueAfterInitialPlug{};
};

/** Resolved fixed native array header before its ordinary-socket entries. */
struct ArrayHeader {
    std::uint64_t repeatedCount{};
    std::uint32_t elementClass{};
    std::array<std::byte, kArrayFirstEntryOffset - sizeof(std::uint64_t) - sizeof(std::uint32_t)>
        opaqueBeforeFirstEntry{};
};

/** Fixed prefix of the optional socket-entry-list selection block. */
struct SocketListBlock {
    std::uint16_t index{};
};

static_assert(offsetof(ItemDefinition, equipmentBlockRelative) == kEquipmentBlockRelativeOffset);
static_assert(offsetof(ItemDefinition, ordinarySocketBlockRelative)
              == kOrdinarySocketBlockRelativeOffset);
static_assert(offsetof(ItemDefinition, socketListBlockRelative) == kSocketListBlockRelativeOffset);
static_assert(offsetof(ItemDefinition, maxStackSize) == kMaxStackSizeOffset);
static_assert(offsetof(ItemDefinition, bucketId) == kBucketIdOffset);
static_assert(offsetof(ItemDefinition, instancedDefinitionPredicate)
              == kInstancedDefinitionPredicateOffset);
static_assert(offsetof(EquipmentBlock, slot) == kEquipmentSlotOffset);
static_assert(offsetof(RelativeArray, headerRelative) == kArrayHeaderRelativeOffset);
static_assert(offsetof(OrdinarySocketEntry, initialPlugIndex) == kInitialPlugIndexOffset);
static_assert(sizeof(OrdinarySocketEntry) == kOrdinarySocketEntrySize);
static_assert(sizeof(ArrayHeader) == kArrayFirstEntryOffset);

} // namespace sunrise::client::content::items::details::layout
