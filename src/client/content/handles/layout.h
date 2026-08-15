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

static_assert(sizeof(std::uintptr_t) == sizeof(std::uint64_t));
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

} // namespace sunrise::client::content::handles::layout
