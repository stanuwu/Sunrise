#include <algorithm>
#include <limits>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Cache padding fields are always written as zero. */
constexpr unsigned int kReservedFieldValue = 0;

} // namespace

/** Encodes one name with zero padding and a fixed-width length. */
bool encode(const content::Definition& value, NamedRecord& record) noexcept {
    if (value.nameLength == 0 || value.nameLength >= value.name.size()
        || value.nameLength > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const auto name = std::span(value.name).first(value.nameLength);
    if (std::find(name.begin(), name.end(), '\0') != name.end()) {
        return false;
    }
    record = {};
    std::copy_n(value.name.begin(), value.nameLength, record.name.begin());
    record.nameLength = static_cast<std::uint16_t>(value.nameLength);
    record.tag = value.tag;
    record.classId = value.classId;
    return true;
}

/** Decodes one name only when every unused byte is still zero. */
bool decode(const NamedRecord& record, content::Definition& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue || record.nameLength == 0
        || record.nameLength >= record.name.size()) {
        return false;
    }
    const auto terminator = record.name.begin() + record.nameLength;
    if (std::find(record.name.begin(), terminator, '\0') != terminator
        || !std::all_of(terminator, record.name.end(), [](char byte) { return byte == '\0'; })) {
        return false;
    }
    std::copy(record.name.begin(), record.name.end(), value.name.begin());
    value.nameLength = record.nameLength;
    value.tag = record.tag;
    value.classId = record.classId;
    return true;
}

/**
 * Encodes one installed-build item mapping with its padding zeroed.
 * @param value Runtime row.
 * @param record Receives the packed disk row.
 * @return Always true, because an unknown bucket has its own unset value.
 */
bool encode(const items::Definition& value, ItemRecord& record) noexcept {
    record = {
        value.definitionHash,
        value.definitionIndex,
        value.bucketId,
        kReservedFieldValue,
    };
    return true;
}

/** Decodes one installed-build item mapping after checking its padding. */
bool decode(const ItemRecord& record, items::Definition& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value = {record.definitionHash, record.definitionIndex, record.bucketId};
    return true;
}

/** Encodes the optional equipment slot without writing the optional's own storage. */
bool encode(const items::details::Definition& value, ItemDetailRecord& record) noexcept {
    if (value.instancedDefinitionState != items::details::InstancedDefinitionState::stackable
        && value.instancedDefinitionState != items::details::InstancedDefinitionState::instanced) {
        return false;
    }
    record = {};
    record.definitionIndex = value.definitionIndex;
    record.bucketId = value.bucketId;
    record.equipmentSlot = value.equipmentSlot.value_or(kAbsentEquipmentSlot);
    record.instancedDefinition = static_cast<std::uint8_t>(value.instancedDefinitionState);
    record.ordinarySocketState = static_cast<std::uint8_t>(value.ordinarySocketState);
    record.ordinarySocketCount = value.ordinarySocketCount;
    record.maxStackSize = value.maxStackSize;
    record.socketEntryListIndex = value.socketEntryListIndex;
    record.initialPlugIndices = value.initialPlugIndices;
    record.socketTypes = value.socketTypes;
    record.statCount = value.statCount;
    for (std::size_t entry = 0; entry < value.stats.size(); ++entry) {
        record.statRows[entry] = value.stats[entry].row;
        record.statValues[entry] = value.stats[entry].value;
    }
    record.definitionHash = value.definitionHash;
    record.gearArtIndex = value.gearArtIndex;
    record.artArrangementIndex = value.artArrangementIndex;
    record.sandboxPerkCount = value.sandboxPerkCount;
    record.sandboxPerks = value.sandboxPerks;
    record.renderOverrideCount = value.renderOverrideCount;
    for (std::size_t entry = 0; entry < value.renderOverrides.size(); ++entry) {
        record.overrideStages[entry] = value.renderOverrides[entry].stage;
        record.overrideKeys[entry] = value.renderOverrides[entry].key;
        record.overrideValues[entry] = value.renderOverrides[entry].value;
    }
    return true;
}

/** Turns the equipment-slot unset value back into a runtime optional. */
bool decode(const ItemDetailRecord& record, items::details::Definition& value) noexcept {
    value = {};
    if (record.instancedDefinition
        > static_cast<std::uint8_t>(items::details::InstancedDefinitionState::instanced)) {
        return false;
    }
    value.definitionIndex = record.definitionIndex;
    value.bucketId = record.bucketId;
    value.maxStackSize = record.maxStackSize;
    value.instancedDefinitionState =
        static_cast<items::details::InstancedDefinitionState>(record.instancedDefinition);
    if (record.equipmentSlot != kAbsentEquipmentSlot) {
        value.equipmentSlot = record.equipmentSlot;
    }
    value.ordinarySocketState =
        static_cast<items::details::OrdinarySocketState>(record.ordinarySocketState);
    value.ordinarySocketCount = record.ordinarySocketCount;
    value.initialPlugIndices = record.initialPlugIndices;
    value.socketTypes = record.socketTypes;
    value.socketEntryListIndex = record.socketEntryListIndex;
    value.statCount = record.statCount;
    for (std::size_t entry = 0; entry < value.stats.size(); ++entry) {
        value.stats[entry] = {record.statRows[entry], record.statValues[entry]};
    }
    value.definitionHash = record.definitionHash;
    value.gearArtIndex = record.gearArtIndex;
    value.artArrangementIndex = record.artArrangementIndex;
    value.sandboxPerkCount = record.sandboxPerkCount;
    value.sandboxPerks = record.sandboxPerks;
    value.renderOverrideCount = record.renderOverrideCount;
    for (std::size_t entry = 0; entry < value.renderOverrides.size(); ++entry) {
        value.renderOverrides[entry] = {
            record.overrideStages[entry], record.overrideKeys[entry], record.overrideValues[entry]};
    }
    return true;
}

/** Encodes one inventory-bucket routing descriptor. */
bool encode(const inventory::buckets::Descriptor& value, InventoryBucketRecord& record) noexcept {
    record = {
        value.bucketId,
        static_cast<std::uint8_t>(value.arraySelector),
        value.firstSlot,
        value.slotCount,
    };
    return true;
}

/** Decodes one inventory-bucket routing descriptor. */
bool decode(const InventoryBucketRecord& record, inventory::buckets::Descriptor& value) noexcept {
    value = {
        record.bucketId,
        static_cast<inventory::buckets::ArraySelector>(record.arraySelector),
        record.firstSlot,
        record.slotCount,
    };
    return true;
}

} // namespace sunrise::state::build_data::cache::records
