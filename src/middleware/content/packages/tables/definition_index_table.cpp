#include "definition_index_table.h"

#include <cstring>
#include <limits>

namespace sunrise::middleware::content::packages::tables {
namespace {

/** The globals container names its children after its 8-byte prefix and first field. */
constexpr std::size_t kChildTableOffset = 16;
/** The investment root puts its slots right after the prefix. */
constexpr std::size_t kSlotTableOffset = 8;
/** Each child is 16 bytes and begins with its tag. */
constexpr std::size_t kChildStride = 16;
/** A container's prefix is not a child. */
constexpr std::size_t kContainerPrefix = 8;
/** Array descriptors are a count and a self-relative offset pair. */
constexpr std::size_t kDescriptorStride = 8;
/** An array header is a count, an element class, then its data. */
constexpr std::size_t kHeaderCountOffset = 0;
constexpr std::size_t kHeaderClassOffset = 8;
constexpr std::size_t kHeaderDataOffset = 16;
/** The marker comes before the header count. */
constexpr std::size_t kHeaderMarkerBack = 4;
/** Tag-like values carry this high half word. */
constexpr std::uint32_t kTagHighHalf = 0x8080;
/** An index-table row is 24 bytes. */
constexpr std::size_t kIndexRowStride = 24;
/** The row's target tag follows its hash and two opaque fields. */
constexpr std::size_t kIndexRowTagOffset = 16;
/** Arrays larger than this are not definition tables. */
constexpr std::uint64_t kMaximumArrayCount = 300000;

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/**
 * Checks one array header reached from a descriptor.
 * @param blob Whole definition bytes.
 * @param headerOffset Candidate header offset.
 * @param count Count declared by the descriptor.
 * @param elementClass Receives the header element class.
 * @return True when the header repeats the count and both class words look like tags.
 */
[[nodiscard]] bool valid_header(std::span<const std::byte> blob,
                                std::size_t headerOffset,
                                std::uint64_t count,
                                std::uint32_t& elementClass) noexcept {
    std::uint64_t headerCount = 0;
    std::uint32_t marker = 0;
    if (headerOffset < kHeaderMarkerBack
        || !read(blob, headerOffset + kHeaderCountOffset, headerCount) || headerCount != count
        || !read(blob, headerOffset - kHeaderMarkerBack, marker)
        || !read(blob, headerOffset + kHeaderClassOffset, elementClass)) {
        return false;
    }
    return (marker >> 16U) == kTagHighHalf && (elementClass >> 16U) == kTagHighHalf;
}

/**
 * Turns one array descriptor into a found array.
 * @param blob Whole definition bytes.
 * @param offset Descriptor offset.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a valid header.
 */
[[nodiscard]] bool
resolve_descriptor(std::span<const std::byte> blob, std::size_t offset, Array& output) noexcept {
    output = {};
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    /** Descriptor bases must fit the signed self-relative offset range. */
    constexpr std::int64_t kMaximumOffset = (std::numeric_limits<std::int64_t>::max)();
    /** Lower bound that rejects a negative self-relative addition before it overflows. */
    constexpr std::int64_t kMinimumOffset = (std::numeric_limits<std::int64_t>::min)();
    if (offset > static_cast<std::size_t>(kMaximumOffset) - kDescriptorStride) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(offset + kDescriptorStride);
    if (!read(blob, offset, count) || count == 0 || count > kMaximumArrayCount
        || !read(blob, offset + kDescriptorStride, relative)) {
        return false;
    }
    // The offset is signed. An array header may sit before the descriptor that names it.
    if ((relative > 0 && base > kMaximumOffset - relative)
        || (relative < 0 && base < kMinimumOffset - relative)) {
        return false;
    }
    const std::int64_t header = base + relative;
    if (header < static_cast<std::int64_t>(kHeaderMarkerBack)
        || static_cast<std::uint64_t>(header) + kHeaderDataOffset > blob.size()) {
        return false;
    }
    const auto headerOffset = static_cast<std::size_t>(header);
    std::uint32_t elementClass = 0;
    if (!valid_header(blob, headerOffset, count, elementClass)) {
        return false;
    }
    output = Array{count, headerOffset + kHeaderDataOffset, elementClass};
    return true;
}

} // namespace

/** Reads the array descriptor at one known offset. */
bool find_array_at(std::span<const std::byte> blob,
                   std::size_t descriptorOffset,
                   Array& output) noexcept {
    return resolve_descriptor(blob, descriptorOffset, output);
}

/** Finds the first array whose header names one element class. */
bool find_array(std::span<const std::byte> blob,
                std::uint32_t elementClass,
                Array& output) noexcept {
    output = {};
    // A descriptor is a count and a self-relative offset, so a shorter blob holds none.
    if (blob.size() <= kDescriptorStride * 2) {
        return false;
    }
    for (std::size_t offset = 0; offset + (kDescriptorStride * 2) <= blob.size();
         offset += kDescriptorStride) {
        Array candidate{};
        if (resolve_descriptor(blob, offset, candidate) && candidate.elementClass == elementClass) {
            output = candidate;
            return true;
        }
    }
    return false;
}

/** @param blob Whole container bytes. @return Its child count. */
std::size_t child_count(std::span<const std::byte> blob) noexcept {
    return blob.size() <= kContainerPrefix ? 0 : (blob.size() - kContainerPrefix) / kChildStride;
}

/** Walks the child tags of one container blob. */
bool child_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept {
    tag = 0;
    return index < child_count(blob) && read(blob, kChildTableOffset + index * kChildStride, tag);
}

/** Reads one investment-root slot. */
bool slot_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept {
    tag = 0;
    return read(blob, kSlotTableOffset + index * kChildStride, tag);
}

/** Reads one row of an index table that was found. */
bool index_row(std::span<const std::byte> blob,
               const Array& array,
               std::uint64_t index,
               IndexRow& row) noexcept {
    row = {};
    if (index >= array.count) {
        return false;
    }
    const std::size_t offset = array.dataOffset + static_cast<std::size_t>(index * kIndexRowStride);
    return read(blob, offset, row.definitionHash)
           && read(blob, offset + kIndexRowTagOffset, row.targetTag);
}

/** Reads every row of one index table in order and hands each to a visitor. */
bool visit_index_rows(std::span<const std::byte> blob,
                      const Array& array,
                      RowVisitor visitor,
                      void* context) noexcept {
    if (visitor == nullptr || array.count == 0) {
        return false;
    }
    const std::uint64_t tableSize = kIndexRowStride * array.count;
    if (tableSize > blob.size() || array.dataOffset > blob.size() - tableSize) {
        return false;
    }
    for (std::uint64_t index = 0; index < array.count; ++index) {
        const std::size_t offset =
            array.dataOffset + static_cast<std::size_t>(index * kIndexRowStride);
        IndexRow row{};
        if (!read(blob, offset, row.definitionHash)
            || !read(blob, offset + kIndexRowTagOffset, row.targetTag)) {
            return false;
        }
        if (!visitor(context, static_cast<std::uint32_t>(index), row)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
