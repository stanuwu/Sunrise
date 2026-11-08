#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::client::content::handles::layout {

/** A loaded handle-table descriptor keeps 32 opaque middle bytes. */
inline constexpr std::size_t kDescriptorMiddleByteCount = 32;
/** A loaded handle-table descriptor keeps 8 opaque trailing bytes. */
inline constexpr std::size_t kDescriptorTailByteCount = 8;
/** One loaded handle-table descriptor is 64 bytes. */
inline constexpr std::size_t kTableDescriptorSize = 64;
/** One loaded handle record holds its correction source in a 16-byte prefix. */
inline constexpr std::size_t kRecordPrefixSize = 16;

/** Runtime descriptor for one encoded loaded-content handle table. */
struct TableDescriptor {
    std::uint64_t opaque00{};
    std::uintptr_t recordArray{};
    std::array<std::byte, kDescriptorMiddleByteCount> opaque16{};
    std::uint32_t recordStride{};
    std::int32_t correctionMask{};
    std::array<std::byte, kDescriptorTailByteCount> opaque56{};
};

/** Runtime prefix used to recover one loaded definition address. */
struct RecordPrefix {
    std::uint64_t opaque00{};
    std::uint64_t correctionSource{};
};

/** Both top bits mark a size in a content slot's first word; a class reference has only one. */
inline constexpr std::uint32_t kSlotSizeForm = 0xC0000000U, kSlotSizeMask = 0x3FFFFFFFU;
/** Type-word bit set while the entry's bytes are still being read. */
inline constexpr std::uint32_t kSlotLoading = 0x100000U;
/** Class word of a slot that is loading or empty. */
inline constexpr std::uint32_t kSlotNoClass = 0xFFFFFFFFU;

/** One package content slot; the record prefix above is its first 16 bytes. */
struct ContentSlot {
    /** Loaded size with `kSlotSizeForm`, or a class reference for a manifest root. */
    std::uint32_t sizeWord{};
    /** The package entry's type word, plus `kSlotLoading`. */
    std::uint32_t typeWord{};
    /** Slot address minus content address; equal to the slot address when empty. */
    std::int64_t link{};
    std::uint32_t classId{};
};

static_assert(sizeof(std::uintptr_t) == sizeof(std::uint64_t));
static_assert(offsetof(ContentSlot, link) == offsetof(RecordPrefix, correctionSource));
static_assert(offsetof(TableDescriptor, recordArray) == sizeof(std::uint64_t));
static_assert(offsetof(TableDescriptor, recordStride)
              == sizeof(std::uint64_t) + sizeof(std::uintptr_t) + kDescriptorMiddleByteCount);
static_assert(offsetof(TableDescriptor, correctionMask)
              == offsetof(TableDescriptor, recordStride) + sizeof(std::uint32_t));
static_assert(offsetof(RecordPrefix, correctionSource) == sizeof(std::uint64_t));
static_assert(sizeof(TableDescriptor) == kTableDescriptorSize);
static_assert(sizeof(RecordPrefix) == kRecordPrefixSize);
static_assert(std::is_trivially_copyable_v<TableDescriptor>);
static_assert(std::is_trivially_copyable_v<RecordPrefix>);
static_assert(std::is_trivially_copyable_v<ContentSlot>);

} // namespace sunrise::client::content::handles::layout
