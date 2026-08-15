#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {

/** Encodes one map-package stem summary. */
bool encode(const spawn_sets::Stem& value, SpawnStemRecord& record) noexcept {
    record = {};
    if (value.nameLength == 0 || value.nameLength > spawn_sets::kStemNameCapacity
        || value.setCount == 0) {
        return false;
    }
    std::copy(value.name.begin(), value.name.end(), record.name.begin());
    record.pointCount = value.pointCount;
    record.setCount = value.setCount;
    record.nameHashOffset = value.nameHashOffset;
    record.nameHashCount = value.nameHashCount;
    record.nameLength = value.nameLength;
    return true;
}

/** Decodes one map-package stem summary. */
bool decode(const SpawnStemRecord& record, spawn_sets::Stem& value) noexcept {
    value = {};
    if (record.nameLength == 0 || record.nameLength > spawn_sets::kStemNameCapacity
        || record.setCount == 0 || record.reserved != 0) {
        return false;
    }
    std::copy(record.name.begin(), record.name.end(), value.name.begin());
    value.pointCount = record.pointCount;
    value.setCount = record.setCount;
    value.nameHashOffset = record.nameHashOffset;
    value.nameHashCount = record.nameHashCount;
    value.nameLength = record.nameLength;
    return true;
}

/** Encodes one distinct spawn-name hash and its point count. */
bool encode(const spawn_sets::NameHash& value, SpawnNameHashRecord& record) noexcept {
    record = {};
    if (value.pointCount == 0) {
        return false;
    }
    record.value = value.value;
    record.pointCount = value.pointCount;
    record.stemIndex = value.stemIndex;
    record.bubbleMask = value.bubbleMask;
    record.unbound = value.unbound;
    record.inMapPackage = value.inMapPackage;
    record.activityPackageCount = value.activityPackageCount;
    record.activityPackageOverflow = value.activityPackageOverflow;
    record.activityPackages = value.activityPackages;
    return true;
}

/** Decodes one distinct spawn-name hash and its point count. */
bool decode(const SpawnNameHashRecord& record, spawn_sets::NameHash& value) noexcept {
    value = {};
    if (record.pointCount == 0 || record.unbound > 1 || record.inMapPackage > 1
        || record.activityPackageOverflow > 1
        || record.activityPackageCount > spawn_sets::kPackageCapacity
        || record.reserved != decltype(record.reserved){}) {
        return false;
    }
    value = {record.value,
             record.pointCount,
             record.stemIndex,
             record.bubbleMask,
             record.unbound,
             record.inMapPackage,
             record.activityPackageCount,
             record.activityPackageOverflow,
             record.activityPackages};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
