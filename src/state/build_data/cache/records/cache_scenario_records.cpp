#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** @return True when every field of the row fits its disk form. */
[[nodiscard]] bool storable(const scenarios::Definition& value) noexcept {
    return value.nameLength != 0 && value.nameLength <= scenarios::kNameCapacity
           && value.bubbleCount <= scenarios::kBubbleCapacity && value.truncated <= 1
           && value.rosterGroupCount <= scenarios::kDestinationGroupCapacity
           && value.spawnStemLength <= scenarios::kSpawnStemCapacity
           && value.packageCount <= scenarios::kDestinationPackageCapacity;
}

/** @return True when every field of the roster group fits its disk form. */
[[nodiscard]] bool storable(const scenarios::RosterGroup& value) noexcept {
    return value.registryKey != 0 && value.slotCount != 0
           && value.slotCount <= scenarios::kRosterSlotCapacity;
}

} // namespace

/**
 * Encodes one extracted destination layout into its packed disk row.
 * @param value Runtime row.
 * @param record Receives the packed disk row.
 * @return True when every field fits its disk form.
 */
bool encode(const scenarios::Definition& value, ScenarioRecord& record) noexcept {
    record = {};
    if (!storable(value)) {
        return false;
    }
    std::copy(value.name.begin(), value.name.end(), record.name.begin());
    record.tag = value.tag;
    record.nameLength = value.nameLength;
    record.bubbleCount = value.bubbleCount;
    record.truncated = value.truncated;
    record.rosterGroupCount = value.rosterGroupCount;
    record.spawnStemLength = value.spawnStemLength;
    std::copy(value.spawnStem.begin(), value.spawnStem.end(), record.spawnStem.begin());
    std::copy(value.bubbleStates.begin(), value.bubbleStates.end(), record.bubbleStates.begin());
    std::copy(value.bubbleHashes.begin(), value.bubbleHashes.end(), record.bubbleHashes.begin());
    std::copy(value.bubbleStateCounts.begin(),
              value.bubbleStateCounts.end(),
              record.bubbleStateCounts.begin());
    std::copy(value.rosterGroups.begin(), value.rosterGroups.end(), record.rosterGroups.begin());
    std::copy(value.bubbleMapIndices.begin(),
              value.bubbleMapIndices.end(),
              record.bubbleMapIndices.begin());
    record.packageCount = value.packageCount;
    std::copy(value.packages.begin(), value.packages.end(), record.packages.begin());
    return true;
}

/**
 * Decodes one packed disk row into its extracted destination layout.
 * @param record Disk row.
 * @param value Receives the runtime row.
 * @return True when the row is in its standard form.
 */
bool decode(const ScenarioRecord& record, scenarios::Definition& value) noexcept {
    value = {};
    if (record.nameLength == 0 || record.nameLength > scenarios::kNameCapacity
        || record.bubbleCount > scenarios::kBubbleCapacity || record.truncated > 1
        || record.rosterGroupCount > scenarios::kDestinationGroupCapacity
        || record.spawnStemLength > scenarios::kSpawnStemCapacity
        || record.packageCount > scenarios::kDestinationPackageCapacity
        || record.packageReserved != 0 || record.reserved != decltype(record.reserved){}) {
        return false;
    }
    std::copy(record.name.begin(), record.name.end(), value.name.begin());
    value.tag = record.tag;
    value.nameLength = record.nameLength;
    value.bubbleCount = record.bubbleCount;
    value.truncated = record.truncated;
    value.rosterGroupCount = record.rosterGroupCount;
    value.spawnStemLength = record.spawnStemLength;
    std::copy(record.spawnStem.begin(), record.spawnStem.end(), value.spawnStem.begin());
    std::copy(record.bubbleStates.begin(), record.bubbleStates.end(), value.bubbleStates.begin());
    std::copy(record.bubbleHashes.begin(), record.bubbleHashes.end(), value.bubbleHashes.begin());
    std::copy(record.bubbleStateCounts.begin(),
              record.bubbleStateCounts.end(),
              value.bubbleStateCounts.begin());
    std::copy(record.rosterGroups.begin(), record.rosterGroups.end(), value.rosterGroups.begin());
    std::copy(record.bubbleMapIndices.begin(),
              record.bubbleMapIndices.end(),
              value.bubbleMapIndices.begin());
    value.packageCount = record.packageCount;
    std::copy(record.packages.begin(), record.packages.end(), value.packages.begin());
    return true;
}

/**
 * Encodes one extracted roster group into its packed disk row.
 * @param value Runtime roster group.
 * @param record Receives the packed disk row.
 * @return True when every field fits its disk form.
 */
bool encode(const scenarios::RosterGroup& value, RosterGroupRecord& record) noexcept {
    record = {};
    if (!storable(value)) {
        return false;
    }
    record.registryKey = value.registryKey;
    record.objectTag = value.objectTag;
    record.slotCount = value.slotCount;
    std::copy(value.slotTypes.begin(), value.slotTypes.end(), record.slotTypes.begin());
    std::copy(value.slotFlags.begin(), value.slotFlags.end(), record.slotFlags.begin());
    return true;
}

/**
 * Decodes one packed disk row into its extracted roster group.
 * @param record Disk row.
 * @param value Receives the runtime roster group.
 * @return True when the row is in its standard form.
 */
bool decode(const RosterGroupRecord& record, scenarios::RosterGroup& value) noexcept {
    value = {};
    if (record.registryKey == 0 || record.slotCount == 0
        || record.slotCount > scenarios::kRosterSlotCapacity) {
        return false;
    }
    value.registryKey = record.registryKey;
    value.objectTag = record.objectTag;
    value.slotCount = record.slotCount;
    std::copy(record.slotTypes.begin(), record.slotTypes.end(), value.slotTypes.begin());
    std::copy(record.slotFlags.begin(), record.slotFlags.end(), value.slotFlags.begin());
    return true;
}

} // namespace sunrise::state::build_data::cache::records
