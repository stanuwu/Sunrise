#include <array>

#include "../../../../middleware/content/packages/tables/ability_pool_reader.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace pool = middleware::content::packages::tables::abilities;
namespace domain = state::build_data::abilities;

/**
 * Socket entry of the sprint ability, the one entry the character cannot choose.
 * The other five come from the character's own selection.
 */
constexpr std::uint8_t kSprintEntry = 1;
/** Number of socket entries the character sheet's summary selects. */
constexpr std::size_t kSummaryEntryCount = 6;
/** Entry kind of the super, which stays active without a plug source of its own. */
constexpr std::uint8_t kSuperKind = 34;
/** A selector chain longer than this is a cycle, not a chain. */
constexpr std::size_t kSelectorChainLimit = 8;

/** Everything one subclass walk needs to read its entries and their pools. */
struct Walk {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    std::vector<std::byte>* blob{};
    std::array<pool::Entry, pool::kEntryCapacity> entries{};
    std::size_t entryCount{};
    std::array<std::uint8_t, kSummaryEntryCount> selected{};
};

/**
 * Orders one character's selection the way the walk reads it.
 * When two entries share a group the first one claims it, so this order is fixed.
 * @param selection The character's 5 selected socket entries.
 * @return The 6 summary entries in claim order.
 */
[[nodiscard]] std::array<std::uint8_t, kSummaryEntryCount>
summary_entries(const domain::Selection& selection) noexcept {
    return {kSprintEntry,
            selection.classEntry,
            selection.movementEntry,
            selection.grenadeEntry,
            selection.superEntry,
            selection.meleeEntry};
}

/**
 * Reads one entry's pool records.
 * @param walk Subclass walk state.
 * @param entry Entry naming the pool.
 * @param subgroup Pool subgroup ordinal.
 * @param records Record storage.
 * @return The number of records read.
 */
[[nodiscard]] std::size_t records_of(const Walk& walk,
                                     const pool::Entry& entry,
                                     std::uint8_t subgroup,
                                     std::span<pool::PoolRecord> records) noexcept {
    if (entry.poolTag == 0
        || !reader::read_tag(*walk.source, *walk.scratch, entry.poolTag, *walk.blob)) {
        return 0;
    }
    return pool::read_pool_records(
        std::span<const std::byte>{*walk.blob}, entry, subgroup, records);
}

/**
 * Follows one entry's selector chain to the bucket it lands in.
 * A record either names its destination bucket or links on to another entry, subgroup and element.
 * @param walk Subclass walk state.
 * @param entryIndex Entry the chain starts at.
 * @param bucket Receives the destination bucket.
 * @return True when the chain reaches a destination inside the bucket range.
 */
[[nodiscard]] bool
selector_destination(const Walk& walk, std::uint8_t entryIndex, std::uint8_t& bucket) noexcept {
    std::uint8_t subgroup = 0;
    std::uint8_t element = 0;
    for (std::size_t step = 0; step < kSelectorChainLimit; ++step) {
        std::array<pool::PoolRecord, pool::kPoolRecordCapacity> records{};
        const std::size_t count =
            entryIndex < walk.entryCount
                ? records_of(walk, walk.entries[entryIndex], subgroup, records)
                : 0;
        if (element >= count) {
            return false;
        }
        const pool::PoolRecord& record = records[element];
        if (record.destination != pool::kEmptyByte) {
            bucket = record.destination;
            return bucket < domain::kBucketCapacity;
        }
        if (record.linkEntry == pool::kEmptyByte || record.linkSubgroup == pool::kEmptyByte
            || record.linkElement == pool::kEmptyByte) {
            return false;
        }
        entryIndex = record.linkEntry;
        subgroup = record.linkSubgroup;
        element = record.linkElement;
    }
    return false;
}

/**
 * Chooses the active plug source of every entry group.
 * An entry group holds alternatives, and the summary selection names which one the character has.
 * @param walk Subclass walk state.
 * @param sources Receives one active plug source per group, keyed by group.
 */
void chosen_sources(const Walk& walk, std::array<std::uint32_t, 256>& sources) noexcept {
    sources.fill(pool::kNoPlugSource);
    for (const std::uint8_t entryIndex : walk.selected) {
        if (entryIndex >= walk.entryCount) {
            continue;
        }
        const pool::Entry& entry = walk.entries[entryIndex];
        if (entry.plugSource != pool::kNoPlugSource
            && sources[entry.group] == pool::kNoPlugSource) {
            sources[entry.group] = entry.plugSource;
        }
    }
}

/**
 * Decides whether one entry contributes its pool's hashes.
 * @param entry Candidate entry.
 * @param sources Active plug source per group.
 * @return True when the entry is the group's active alternative, or is the super.
 */
[[nodiscard]] bool active(const pool::Entry& entry,
                          const std::array<std::uint32_t, 256>& sources) noexcept {
    if (entry.plugSource == pool::kNoPlugSource) {
        return entry.kind == kSuperKind;
    }
    return sources[entry.group] == entry.plugSource;
}

/**
 * Assigns each selected entry's bucket and the kind that bucket collects.
 * @param walk Subclass walk state.
 * @param output Receives the twelve bucket kinds.
 * @return True when every selected entry reaches a distinct bucket.
 */
[[nodiscard]] bool assign_kinds(const Walk& walk, domain::Definition& output) noexcept {
    for (const std::uint8_t entryIndex : walk.selected) {
        if (entryIndex >= walk.entryCount) {
            return false;
        }
        std::array<pool::PoolRecord, pool::kPoolRecordCapacity> records{};
        std::uint8_t bucket = 0;
        if (records_of(walk, walk.entries[entryIndex], 0, records) == 0
            || records[0].kind == pool::kEmptyByte
            || !selector_destination(walk, entryIndex, bucket)
            || output.buckets[bucket].kind != domain::kEmptyBucketKind) {
            return false;
        }
        output.buckets[bucket].kind = records[0].kind;
    }
    return true;
}

/**
 * Files one pool record's hash into the bucket its category names, or into the overflow bank.
 * @param record Pool record carrying a definition hash.
 * @param output Row receiving the hash.
 */
void file_hash(const pool::PoolRecord& record, domain::Definition& output) noexcept {
    if (record.definitionHash == pool::kNoPlugSource) {
        return;
    }
    if (record.category == pool::kEmptyByte) {
        if (output.overflowCount < output.overflow.size()) {
            output.overflow[output.overflowCount++] = record.definitionHash;
        }
        return;
    }
    for (domain::Bucket& bucket : output.buckets) {
        if (bucket.kind == record.category && bucket.hashCount < bucket.hashes.size()) {
            bucket.hashes[bucket.hashCount++] = record.definitionHash;
        }
    }
}

} // namespace

/** Builds the ability buckets one subclass publishes under one ability selection. */
bool build_ability_buckets(const reader::Source& source,
                           reader::Scratch& scratch,
                           std::span<const std::byte> listDefinition,
                           std::vector<std::byte>& blob,
                           const state::build_data::abilities::Selection& selection,
                           state::build_data::abilities::Definition& output) noexcept {
    Walk walk{};
    walk.source = &source;
    walk.scratch = &scratch;
    walk.blob = &blob;
    walk.entryCount = pool::read_entries(listDefinition, walk.entries);
    if (walk.entryCount == 0) {
        return false;
    }
    walk.selected = summary_entries(selection);

    for (domain::Bucket& bucket : output.buckets) {
        bucket = {};
    }
    output.overflowCount = 0;
    if (!assign_kinds(walk, output)) {
        return false;
    }
    // Kinds must be complete before any hash is filed, because a hash is routed by matching its
    // category against a bucket's kind.
    std::array<std::uint32_t, 256> sources{};
    chosen_sources(walk, sources);
    for (std::size_t entryIndex = 0; entryIndex < walk.entryCount; ++entryIndex) {
        if (!active(walk.entries[entryIndex], sources)) {
            continue;
        }
        std::array<pool::PoolRecord, pool::kPoolRecordCapacity> records{};
        const std::size_t count = records_of(walk, walk.entries[entryIndex], 0, records);
        for (std::size_t entry = 0; entry < count; ++entry) {
            file_hash(records[entry], output);
        }
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
