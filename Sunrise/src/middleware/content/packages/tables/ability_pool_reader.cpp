#include "ability_pool_reader.h"

#include <cstddef>
#include <limits>

#include "../schema/generated/tag_classes_80807a.h"
#include "../schema/generated/tag_classes_80809f.h"
#include "definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables::abilities {
namespace {

namespace generated = schema::generated;
/** Pool blob root, one group, one variant and one record. */
using Pool = generated::C80807a8b;
using Group = generated::C80807a8d;
using Variant = generated::C80807a97;
using Record = generated::AbilityPoolRecord;

/** Entry field offsets this pass reads beyond the shared socket entry layout. */
constexpr std::size_t kEntrySelector = offsetof(generated::SocketEntry, selector);
constexpr std::size_t kEntryPool = offsetof(generated::SocketEntry, pool);

/** The pool blob declares its group count and its group array here. */
constexpr std::size_t kPoolGroupCount = offsetof(Pool, stepList.count);
constexpr std::size_t kPoolGroupArray = offsetof(Pool, stepList.relative);
/** One pool group after the array header. */
constexpr std::size_t kPoolGroupStride = sizeof(Group);
/** A group declares its variant count first and its variant array offset after it. */
constexpr std::size_t kGroupVariantCount = offsetof(Group, h6ea4a4fb.count);
constexpr std::size_t kGroupVariantOffset = offsetof(Group, h6ea4a4fb.relative);
/** One variant row. */
constexpr std::size_t kVariantStride = sizeof(Variant);
/** A variant holds its record array descriptor here. */
constexpr std::size_t kVariantRecordField = offsetof(Variant, abilityList.relative);
/** A variant holds its sandbox perk index array descriptor here. */
constexpr std::size_t kVariantPerkField = offsetof(Variant, perkList.relative);
/** Both variant arrays store their count this far before their self-relative offset. */
constexpr std::size_t kVariantCountBack =
    offsetof(Variant, abilityList.relative) - offsetof(Variant, abilityList.count);
static_assert(offsetof(Variant, perkList.relative) - offsetof(Variant, perkList.count)
              == kVariantCountBack);
/** One pool record. */
constexpr std::size_t kPoolRecordStride = sizeof(Record);
/**
 * Runtime state that picks the pool variant.
 * A pool that declares one group and whose entry carries a live selector picks its variant by
 * this state rather than taking the last one.
 */
constexpr std::int32_t kActiveStateSelector = 18;
/** A pool declaring exactly one group takes the selector path. */
constexpr std::int64_t kSingleGroupPool = 1;
/** Array counts are 64-bit; the variant pick takes a count that fits 32 bits. */
constexpr std::int64_t kMaximumCount = (std::numeric_limits<std::int32_t>::max)();

/** Record field offsets inside one pool record. */
constexpr std::size_t kRecordHash = offsetof(Record, definitionHash);
constexpr std::size_t kRecordCategory = offsetof(Record, category);
constexpr std::size_t kRecordLinkEntry = offsetof(Record, linkEntry);
constexpr std::size_t kRecordLinkSubgroup = offsetof(Record, linkSubgroup);
constexpr std::size_t kRecordLinkElement = offsetof(Record, linkElement);
constexpr std::size_t kRecordKind = offsetof(Record, kind);
constexpr std::size_t kRecordDestination = offsetof(Record, destination);
// Each record field is read at the width of the view member it names.
static_assert(sizeof(Record::category) == sizeof(PoolRecord::category)
              && sizeof(Record::destination) == sizeof(PoolRecord::destination));

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

/**
 * Finds the record array or the sandbox perk array of an entry's active pool variant.
 * A pool declares several variants. Which is active depends on the group count and on whether
 * the entry carries a selector, so the entry is needed to read its own pool.
 * @param blob Whole pool bytes.
 * @param entry Entry naming the pool.
 * @param subgroup Pool subgroup ordinal.
 * @param field Offset of the array's self-relative member inside a variant.
 * @param elements Receives the absolute offset of the first element.
 * @return The element count, or 0 when the pool, the subgroup or the array does not read.
 */
[[nodiscard]] std::size_t variant_array(std::span<const std::byte> blob,
                                        const Entry& entry,
                                        std::uint8_t subgroup,
                                        std::size_t field,
                                        std::size_t& elements) noexcept {
    std::int64_t groupCount = 0;
    std::size_t groupArray = 0;
    if (!read(blob, kPoolGroupCount, groupCount) || !follow(blob, kPoolGroupArray, groupArray)) {
        return 0;
    }
    const std::size_t group = groupArray + reader::layout::TypedArrayHeader::kSize
                              + static_cast<std::size_t>(subgroup) * kPoolGroupStride;
    std::int64_t variantCount = 0;
    std::size_t variantArray = 0;
    if (!read(blob, group + kGroupVariantCount, variantCount) || variantCount <= 0
        || variantCount > kMaximumCount
        || !follow(blob, group + kGroupVariantOffset, variantArray)) {
        return 0;
    }
    const auto variant = static_cast<std::size_t>(
        active_variant(groupCount, static_cast<std::int32_t>(variantCount), entry));
    const std::size_t member =
        variantArray + reader::layout::TypedArrayHeader::kSize + variant * kVariantStride + field;
    std::int64_t count = 0;
    if (member < kVariantCountBack || !read(blob, member - kVariantCountBack, count) || count <= 0
        || count > kMaximumCount || !follow(blob, member, elements)) {
        return 0;
    }
    elements += reader::layout::TypedArrayHeader::kSize;
    return static_cast<std::size_t>(count);
}

} // namespace

/** Reads every entry of one socket-entry-list definition blob. */
std::size_t read_entries(std::span<const std::byte> blob, std::span<Entry> output) noexcept {
    Array entries{};
    if (!find_array_at(blob, kSocketEntryArrayDescriptor, entries)) {
        return 0;
    }
    std::size_t count = 0;
    while (count < entries.count && count < output.size()) {
        const std::size_t base = entries.dataOffset + count * kSocketEntrySize;
        Entry entry{};
        if (!read(blob, base + kSocketEntryPlugSource, entry.plugSource)
            || !read(blob, base + kSocketEntryGroup, entry.group)
            || !read(blob, base + kSocketEntryKind, entry.kind)
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
    std::size_t records = 0;
    const std::size_t recordCount =
        variant_array(blob, entry, subgroup, kVariantRecordField, records);
    std::size_t count = 0;
    while (count < recordCount && count < output.size()) {
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

/** Reads the sandbox perks one entry's active pool variant declares. */
std::size_t read_pool_perks(std::span<const std::byte> blob,
                            const Entry& entry,
                            std::uint8_t subgroup,
                            std::span<std::uint16_t> output) noexcept {
    std::size_t perks = 0;
    const std::size_t perkCount = variant_array(blob, entry, subgroup, kVariantPerkField, perks);
    std::size_t count = 0;
    while (count < perkCount && count < output.size()
           && read(blob, perks + count * sizeof(std::uint16_t), output[count])) {
        ++count;
    }
    return count;
}

} // namespace sunrise::middleware::content::packages::tables::abilities
