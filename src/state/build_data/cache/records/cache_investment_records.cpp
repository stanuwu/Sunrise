#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Cache padding fields are always written as zero. */
constexpr unsigned int kReservedFieldValue = 0;

} // namespace

/** Flattens the 12 buckets into 3 parallel arrays with no per-bucket padding. */
bool encode(const abilities::Definition& value, AbilityBucketRecord& record) noexcept {
    record = {};
    record.socketEntryListIndex = value.socketEntryListIndex;
    record.movementEntry = value.selection.movementEntry;
    record.grenadeEntry = value.selection.grenadeEntry;
    record.superEntry = value.selection.superEntry;
    record.meleeEntry = value.selection.meleeEntry;
    record.classEntry = value.selection.classEntry;
    record.overflowCount = value.overflowCount;
    record.overflow = value.overflow;
    for (std::size_t bucket = 0; bucket < abilities::kBucketCapacity; ++bucket) {
        record.bucketKinds[bucket] = value.buckets[bucket].kind;
        record.bucketHashCounts[bucket] = value.buckets[bucket].hashCount;
        for (std::size_t entry = 0; entry < abilities::kBucketHashCapacity; ++entry) {
            record.bucketHashes[bucket * abilities::kBucketHashCapacity + entry] =
                value.buckets[bucket].hashes[entry];
        }
    }
    return true;
}

/** Rebuilds the 12 buckets from the flat disk arrays. */
bool decode(const AbilityBucketRecord& record, abilities::Definition& value) noexcept {
    value = {};
    if (record.overflowCount > abilities::kOverflowCapacity) {
        return false;
    }
    value.socketEntryListIndex = record.socketEntryListIndex;
    value.selection.movementEntry = record.movementEntry;
    value.selection.grenadeEntry = record.grenadeEntry;
    value.selection.superEntry = record.superEntry;
    value.selection.meleeEntry = record.meleeEntry;
    value.selection.classEntry = record.classEntry;
    value.overflowCount = record.overflowCount;
    value.overflow = record.overflow;
    for (std::size_t bucket = 0; bucket < abilities::kBucketCapacity; ++bucket) {
        if (record.bucketHashCounts[bucket] > abilities::kBucketHashCapacity) {
            return false;
        }
        value.buckets[bucket].kind = record.bucketKinds[bucket];
        value.buckets[bucket].hashCount = record.bucketHashCounts[bucket];
        for (std::size_t entry = 0; entry < abilities::kBucketHashCapacity; ++entry) {
            value.buckets[bucket].hashes[entry] =
                record.bucketHashes[bucket * abilities::kBucketHashCapacity + entry];
        }
    }
    return true;
}

/** Encodes one progression definition with its padding zeroed. */
bool encode(const progressions::Definition& value, ProgressionRecord& record) noexcept {
    record = {
        value.definitionIndex,
        static_cast<std::uint8_t>(value.scope),
        kReservedFieldValue,
    };
    return true;
}

/** Decodes one progression definition after checking its padding. */
bool decode(const ProgressionRecord& record, progressions::Definition& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value = {record.definitionIndex, static_cast<progressions::Scope>(record.scope)};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
