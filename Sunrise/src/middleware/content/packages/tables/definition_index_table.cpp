#include "definition_index_table.h"

#include <limits>

#include "../reader/layout.h"
#include "../reader/typed_array.h"
#include "../schema/generated/tag_classes_80805b.h"
#include "../schema/generated/tag_classes_808078.h"
#include "../schema/generated/tag_classes_80807a.h"
#include "../schema/generated/tag_classes_80807b.h"
#include "../schema/generated/tag_classes_80807d.h"
#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

namespace generated = schema::generated;
/** Array descriptors are a count and a self-relative offset pair. */
using Descriptor = generated::TagArray<generated::Unknown>;

/** A container's prefix is not a child. */
constexpr std::size_t kContainerPrefix = offsetof(generated::InvestmentGlobals, unk8);
/** Descriptors are 8-byte aligned, so a scan steps by that. */
constexpr std::size_t kDescriptorStride = alignof(Descriptor);

/** Every index table shares the item index row layout. */
constexpr std::size_t kIndexRowStride = sizeof(generated::ItemIndexRow);
constexpr std::size_t kIndexRowHashOffset = offsetof(generated::ItemIndexRow, itemHash);
/** The row's target tag follows its hash and two opaque fields. */
constexpr std::size_t kIndexRowTagOffset = offsetof(generated::ItemIndexRow, itemDefinition);
// The vendor and socket entry list indexes are read through the item index row layout.
static_assert(sizeof(generated::VendorIndexRow) == kIndexRowStride
              && offsetof(generated::VendorIndexRow, vendorHash) == kIndexRowHashOffset
              && offsetof(generated::VendorIndexRow, definitionTag) == kIndexRowTagOffset);
static_assert(sizeof(generated::SocketEntryList) == kIndexRowStride
              && offsetof(generated::SocketEntryList, unk0) == kIndexRowHashOffset
              && offsetof(generated::SocketEntryList, entries) == kIndexRowTagOffset);
/** Arrays larger than this are not definition tables. */
constexpr std::uint64_t kMaximumArrayCount = 300000;

/**
 * Turns one array descriptor into a found array.
 * @param blob Whole definition bytes.
 * @param offset Descriptor offset.
 * @param allowEmpty Accepts a zero count, with or without a header.
 * @param elementClass Class a non-empty array's header must name, or kAnyElementClass.
 * @param stride Row size the rows must fit at; zero skips the row bound.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a checked typed-array header.
 */
[[nodiscard]] bool resolve_descriptor(std::span<const std::byte> blob,
                                      std::size_t offset,
                                      bool allowEmpty,
                                      std::uint32_t elementClass,
                                      std::size_t stride,
                                      Array& output) noexcept {
    output = {};
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    /** Descriptor bases must fit the signed self-relative offset range. */
    constexpr std::int64_t kMaximumOffset = (std::numeric_limits<std::int64_t>::max)();
    /** Lower bound that rejects a negative self-relative addition before it overflows. */
    constexpr std::int64_t kMinimumOffset = (std::numeric_limits<std::int64_t>::min)();
    if (offset > static_cast<std::size_t>(kMaximumOffset) - reader::layout::kArrayFieldRelative) {
        return false;
    }
    const std::int64_t base =
        static_cast<std::int64_t>(offset + reader::layout::kArrayFieldRelative);
    if (!read(blob, offset, count) || count > kMaximumArrayCount
        || !read(blob, offset + reader::layout::kArrayFieldRelative, relative)) {
        return false;
    }
    if (count == 0) {
        // An array with no entries is authored either with no header at all or with a header
        // that repeats the zero count. A caller scanning for an element class can use neither,
        // so it asks for the empty forms to be refused.
        if (!allowEmpty) {
            return false;
        }
        if (relative == 0) {
            return true;
        }
    }
    // The offset is signed. An array header may sit before the descriptor that names it.
    if ((relative > 0 && base > kMaximumOffset - relative)
        || (relative < 0 && base < kMinimumOffset - relative)) {
        return false;
    }
    const std::int64_t header = base + relative;
    if (header < 0) {
        return false;
    }
    reader::TypedArrayRows rows{};
    if (!reader::read_typed_array(blob,
                                  static_cast<std::size_t>(header),
                                  count,
                                  count == 0 ? reader::kAnyElementClass : elementClass,
                                  stride,
                                  rows)) {
        return false;
    }
    output = Array{count, rows.data, rows.elementClass};
    return true;
}

} // namespace

/** Reads the array descriptor at one known offset. */
bool find_array_at(std::span<const std::byte> blob,
                   std::size_t descriptorOffset,
                   Array& output) noexcept {
    return resolve_descriptor(blob, descriptorOffset, false, reader::kAnyElementClass, 0, output);
}

/** Reads the array descriptor at one known offset and accepts an authored empty array. */
bool find_optional_array_at(std::span<const std::byte> blob,
                            std::size_t descriptorOffset,
                            Array& output) noexcept {
    return resolve_descriptor(blob, descriptorOffset, true, reader::kAnyElementClass, 0, output);
}

/** Reads an optional typed array and checks every fixed-stride row lies in its blob. */
bool read_array(std::span<const std::byte> blob,
                std::size_t at,
                std::uint32_t elementClass,
                std::size_t stride,
                Array& rows) noexcept {
    return stride != 0 && resolve_descriptor(blob, at, true, elementClass, stride, rows);
}

/** Finds the first array whose header names one element class. */
bool find_array(std::span<const std::byte> blob,
                std::uint32_t elementClass,
                Array& output) noexcept {
    output = {};
    // A descriptor is a count and a self-relative offset, so a shorter blob holds none.
    if (blob.size() <= sizeof(Descriptor)) {
        return false;
    }
    for (std::size_t offset = 0; offset + sizeof(Descriptor) <= blob.size();
         offset += kDescriptorStride) {
        Array candidate{};
        if (resolve_descriptor(blob, offset, false, elementClass, 0, candidate)) {
            output = candidate;
            return true;
        }
    }
    return false;
}

/** @param blob Whole container bytes. @return Its child count. */
std::size_t child_count(std::span<const std::byte> blob) noexcept {
    return blob.size() <= kContainerPrefix ? 0
                                           : (blob.size() - kContainerPrefix) / kGlobalsChildStride;
}

/** Walks the child tags of one container blob. */
bool child_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept {
    tag = 0;
    return index < child_count(blob)
           && read(blob, kGlobalsChildTableOffset + index * kGlobalsChildStride, tag);
}

/** Reads one investment-root slot. */
bool slot_tag(std::span<const std::byte> blob, std::size_t index, std::uint32_t& tag) noexcept {
    tag = 0;
    return read(blob, kRootSlotTableOffset + index * kRootSlotStride, tag);
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
    return read(blob, offset + kIndexRowHashOffset, row.definitionHash)
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
        if (!read(blob, offset + kIndexRowHashOffset, row.definitionHash)
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
