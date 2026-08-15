#include "ability_pool_reader.h"

#include <cstring>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables::abilities {
namespace {

/** A socket-entry-list definition holds its entry array descriptor here. */
constexpr std::size_t kEntryDescriptor = 16;
/** One socket-entry-list entry is 64 bytes. */
constexpr std::size_t kEntryStride = 64;
/** Entry field offsets. */
constexpr std::size_t kEntryPlugSource = 8;
constexpr std::size_t kEntryGroup = 12;
constexpr std::size_t kEntryKind = 13;
constexpr std::size_t kEntrySelector = 32;
constexpr std::size_t kEntryPool = 56;

/** The pool blob declares its group count and its group array here. */
constexpr std::size_t kPoolGroupCount = 8;
constexpr std::size_t kPoolGroupArray = 16;
/** One pool group is 144 bytes after the array header. */
constexpr std::size_t kPoolGroupStride = 144;
/** The group array header is 16 bytes wide. */
constexpr std::size_t kPoolGroupHeader = 16;
/** A group declares its variant count first and its variant array offset 8 bytes later. */
constexpr std::size_t kGroupVariantOffset = 8;
/** The variant array begins 32 bytes into the group. */
constexpr std::size_t kGroupVariantArray = 32;
/** One variant is 88 bytes. */
constexpr std::size_t kVariantStride = 88;
/** A variant's record count comes before its self-relative record offset. */
constexpr std::size_t kVariantCountBack = 8;
/** The record array begins 16 bytes past the variant's own offset field. */
constexpr std::size_t kVariantRecordHeader = 16;
/** One pool record is 16 bytes. */
constexpr std::size_t kPoolRecordStride = 16;
/**
 * Runtime state that picks the pool variant.
 * A pool that declares one group and whose entry carries a live selector picks its variant by
 * this state rather than taking the last one.
 */
constexpr std::int32_t kActiveStateSelector = 18;
/** A pool declaring exactly one group takes the selector path. */
constexpr std::int64_t kSingleGroupPool = 1;

/** Record field offsets inside one 16-byte pool record. */
constexpr std::size_t kRecordHash = 0;
constexpr std::size_t kRecordCategory = 4;
constexpr std::size_t kRecordLinkEntry = 8;
constexpr std::size_t kRecordLinkSubgroup = 9;
constexpr std::size_t kRecordLinkElement = 10;
constexpr std::size_t kRecordKind = 11;
constexpr std::size_t kRecordDestination = 12;

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
 * Turns a self-relative offset into an absolute blob offset.
 * @param blob Source bytes.
 * @param member Offset of the self-relative field.
 * @param resolved Receives the absolute offset.
 * @return True when the field reads and lands inside the blob.
 */
[[nodiscard]] bool
follow(std::span<const std::byte> blob, std::size_t member, std::size_t& resolved) noexcept {
    std::int64_t relative = 0;
    if (!read(blob, member, relative)) {
        return false;
    }
    const std::int64_t target = static_cast<std::int64_t>(member) + relative;
    if (target < 0 || static_cast<std::uint64_t>(target) >= blob.size()) {
        return false;
    }
    resolved = static_cast<std::size_t>(target);
    return true;
}

/**
 * Finds the active variant of one pool group.
 * @param groupCount Groups the pool declares.
 * @param variantCount Variants the group declares.
 * @param entry Entry naming the pool.
 * @return The active variant ordinal.
 */
[[nodiscard]] std::int32_t
active_variant(std::int64_t groupCount, std::int32_t variantCount, const Entry& entry) noexcept {
    if (groupCount == kSingleGroupPool && entry.hasSelector && entry.selector != kEmptyByte) {
        return kActiveStateSelector % variantCount;
    }
    return variantCount - 1;
}

} // namespace

/** Reads every entry of one socket-entry-list definition blob. */
std::size_t read_entries(std::span<const std::byte> blob, std::span<Entry> output) noexcept {
    Array entries{};
    if (!find_array_at(blob, kEntryDescriptor, entries)) {
        return 0;
    }
    std::size_t count = 0;
    while (count < entries.count && count < output.size()) {
        const std::size_t base = entries.dataOffset + count * kEntryStride;
        Entry entry{};
        if (!read(blob, base + kEntryPlugSource, entry.plugSource)
            || !read(blob, base + kEntryGroup, entry.group)
            || !read(blob, base + kEntryKind, entry.kind)
            || !read(blob, base + kEntryPool, entry.poolTag)) {
            break;
        }
        // The selector is behind a self-relative field that is zero when the entry has none.
        std::int64_t selectorRelative = 0;
        std::size_t selectorOffset = 0;
        if (read(blob, base + kEntrySelector, selectorRelative) && selectorRelative != 0
            && follow(blob, base + kEntrySelector, selectorOffset)) {
            entry.hasSelector = read(blob, selectorOffset, entry.selector);
        }
        output[count++] = entry;
    }
    return count;
}

/** Reads the active records of one entry's pool blob. */
std::size_t read_pool_records(std::span<const std::byte> blob,
                              const Entry& entry,
                              std::uint8_t subgroup,
                              std::span<PoolRecord> output) noexcept {
    std::int64_t groupCount = 0;
    std::size_t groupArray = 0;
    if (!read(blob, kPoolGroupCount, groupCount) || !follow(blob, kPoolGroupArray, groupArray)) {
        return 0;
    }
    const std::size_t group =
        groupArray + kPoolGroupHeader + static_cast<std::size_t>(subgroup) * kPoolGroupStride;
    std::int32_t variantCount = 0;
    std::size_t variantArray = 0;
    if (!read(blob, group, variantCount) || variantCount <= 0
        || !follow(blob, group + kGroupVariantOffset, variantArray)) {
        return 0;
    }
    const auto variant = static_cast<std::size_t>(active_variant(groupCount, variantCount, entry));
    const std::size_t variantField =
        variantArray + kGroupVariantArray - kGroupVariantOffset + variant * kVariantStride;
    std::int32_t recordCount = 0;
    std::size_t records = 0;
    if (variantField < kVariantCountBack
        || !read(blob, variantField - kVariantCountBack, recordCount) || recordCount <= 0
        || !follow(blob, variantField, records)) {
        return 0;
    }
    records += kVariantRecordHeader;
    std::size_t count = 0;
    while (count < static_cast<std::size_t>(recordCount) && count < output.size()) {
        const std::size_t base = records + count * kPoolRecordStride;
        PoolRecord record{};
        if (!read(blob, base + kRecordHash, record.definitionHash)
            || !read(blob, base + kRecordCategory, record.category)
            || !read(blob, base + kRecordLinkEntry, record.linkEntry)
            || !read(blob, base + kRecordLinkSubgroup, record.linkSubgroup)
            || !read(blob, base + kRecordLinkElement, record.linkElement)
            || !read(blob, base + kRecordKind, record.kind)
            || !read(blob, base + kRecordDestination, record.destination)) {
            break;
        }
        output[count++] = record;
    }
    return count;
}

} // namespace sunrise::middleware::content::packages::tables::abilities
