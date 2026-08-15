#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::content::items::layout {

/** A loaded item-table blob stores its row count after 8 header bytes. */
inline constexpr std::size_t kTableRowCountOffset = 8;
/** Item index rows begin 48 bytes into the loaded table blob. */
inline constexpr std::size_t kTableFirstRowOffset = 48;
/** Each item index row stores its target handle at byte 16. */
inline constexpr std::size_t kRowTargetHandleOffset = 16;
/** Native item index rows are 24 bytes. */
inline constexpr std::size_t kItemIndexRowSize = 24;
/** A loaded gameplay definition stores its inventory bucket at byte 184. */
inline constexpr std::size_t kDefinitionBucketOffset = 184;

/** One dense native item index row. */
struct ItemIndexRow {
    std::uint32_t definitionHash{};
    std::array<std::byte, kRowTargetHandleOffset - sizeof(std::uint32_t)> opaque04{};
    std::uint32_t targetHandle{};
    std::array<std::byte, kItemIndexRowSize - kRowTargetHandleOffset - sizeof(std::uint32_t)>
        opaque20{};
};

/** Fixed header and first row of the loaded item table. */
struct ItemTable {
    std::array<std::byte, kTableRowCountOffset> opaque00{};
    std::uint64_t rowCount{};
    std::array<std::byte, kTableFirstRowOffset - kTableRowCountOffset - sizeof(std::uint64_t)>
        opaque16{};
    ItemIndexRow firstRow{};
};

/** Fixed prefix through the gameplay definition's bucket field. */
struct ItemDefinition {
    std::array<std::byte, kDefinitionBucketOffset> opaque00{};
    std::uint8_t bucketId{};
};

static_assert(offsetof(ItemIndexRow, targetHandle) == kRowTargetHandleOffset);
static_assert(sizeof(ItemIndexRow) == kItemIndexRowSize);
static_assert(offsetof(ItemTable, rowCount) == kTableRowCountOffset);
static_assert(offsetof(ItemTable, firstRow) == kTableFirstRowOffset);
static_assert(offsetof(ItemDefinition, bucketId) == kDefinitionBucketOffset);

} // namespace sunrise::client::content::items::layout
