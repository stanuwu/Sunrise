#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::datagen::family4::inventory::layout {

/** 6 reserved bytes align the instance SOID on byte 8 of a packed inventory row. */
inline constexpr std::size_t kDefinitionPaddingSize = 6;
/** 4 reserved bytes complete the fixed 32-byte inventory row. */
inline constexpr std::size_t kTailPaddingSize = 4;
/** One native character-inventory row is exactly 32 bytes. */
inline constexpr std::size_t kEntrySize = 32;
/** The biased item-definition index begins at byte 0 of an inventory row. */
inline constexpr std::size_t kDefinitionIndexOffset = 0;
/** The optional item-instance SOID begins at byte 8 of an inventory row. */
inline constexpr std::size_t kInstanceSoidOffset = 8;
/** The stack quantity begins at byte 16 of an inventory row. */
inline constexpr std::size_t kQuantityOffset = 16;
/** The mutation-order serial begins at byte 20 of an inventory row. */
inline constexpr std::size_t kMutationSerialOffset = 20;
/** The accumulated item-state flags begin at byte 24 of an inventory row. */
inline constexpr std::size_t kFlagsOffset = 24;

#pragma pack(push, 1)

/** One fixed native inventory row whose biased definition index marks occupancy. */
struct Entry {
    /** Biased item-definition index; all bits set marks an unused row. */
    std::uint16_t definitionIndex{};
    std::array<std::byte, kDefinitionPaddingSize> definitionPadding{};
    /** Stable item-instance SOID, or 0 when the item is not instanced. */
    std::uint64_t instanceSoid{};
    /** Current stack quantity, set from the item definition for non-instanced rows. */
    std::int32_t quantity{};
    /** Rising serial that keeps inventory mutation and eviction order. */
    std::int32_t mutationSerial{};
    /** Native item-state bits accumulated by inventory mutations. */
    std::uint32_t flags{};
    std::array<std::byte, kTailPaddingSize> tailPadding{};
};

#pragma pack(pop)

static_assert(sizeof(Entry) == kEntrySize);
static_assert(std::is_standard_layout_v<Entry>);
static_assert(std::is_trivially_copyable_v<Entry>);
static_assert(offsetof(Entry, definitionIndex) == kDefinitionIndexOffset);
static_assert(offsetof(Entry, instanceSoid) == kInstanceSoidOffset);
static_assert(offsetof(Entry, quantity) == kQuantityOffset);
static_assert(offsetof(Entry, mutationSerial) == kMutationSerialOffset);
static_assert(offsetof(Entry, flags) == kFlagsOffset);

} // namespace sunrise::middleware::datagen::family4::inventory::layout
