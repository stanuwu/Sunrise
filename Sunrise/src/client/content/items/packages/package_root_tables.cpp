#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/socket_entry_lists/definition.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace buckets = state::build_data::inventory::buckets;

/** The installed equipment-slot catalogue has rows 0 through 18. */
constexpr std::uint8_t kEquipmentSlotCount = 19;
/** Slot 16 is not backed by an inventory bucket in the installed equipment ABI. */
constexpr std::uint8_t kUnavailableBucket = 0xFF;
/**
 * Inventory buckets are storage ranges; equipment slots are render/loadout positions. Their
 * installed-build ABI is independent of item definitions and routes every item through its bucket.
 */
constexpr std::array<std::uint8_t, kEquipmentSlotCount> kBucketByEquipmentSlot{
    16, 3, 4, 36, 5, 6, 7, 0, 1, 2, 10, 9, 8, 27, 41, 17, kUnavailableBucket, 47, 49};

/** @return The unique descriptor carrying one bucket id, or null. */
[[nodiscard]] const buckets::Descriptor*
find_bucket(std::span<const buckets::Descriptor> descriptors, std::uint8_t bucketId) noexcept {
    const buckets::Descriptor* found = nullptr;
    for (const buckets::Descriptor& descriptor : descriptors) {
        if (descriptor.bucketId != bucketId) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = &descriptor;
    }
    return found;
}

/** The first tag a class sweep reported and how many entries carried the class. */
struct BucketDefinitionTable {
    std::uint32_t tag{};
    std::size_t matches{};
};

/** @param context Sweep result. @param tag One matching tag. @return Always true, to count all. */
bool collect_bucket_definition_tag(void* context, std::uint32_t tag) noexcept {
    auto* located = static_cast<BucketDefinitionTable*>(context);
    if (located->matches == 0) {
        located->tag = tag;
    }
    ++located->matches;
    return true;
}

/**
 * Locates the bucket-definition table by the class its entry record carries.
 * A repack moves the tag but not the class, so search by class. Entry tables need no block keys.
 * @param source Package source.
 * @param tag Receives the located tag.
 * @return True when exactly one installed entry carries the class.
 */
[[nodiscard]] bool find_bucket_definition_table(const reader::Source& source,
                                                std::uint32_t& tag) noexcept {
    BucketDefinitionTable located{};
    reader::ScanResult scanned{};
    if (!reader::scan_class(source,
                            tables::kBucketDefinitionTableClass,
                            &collect_bucket_definition_tag,
                            &located,
                            scanned)
        || located.matches != 1) {
        report_bucket_equipment_failure("table_sweep", located.matches, scanned.packages);
        return false;
    }
    tag = located.tag;
    return true;
}

/**
 * Extracts and validates the installed bucket/equipment-slot relation.
 * The table contains inline 72-byte records; its array elements are not index rows or tag links.
 */
[[nodiscard]] bool read_bucket_equipment_slots(
    const reader::Source& source,
    Storage& storage,
    std::span<const buckets::Descriptor> descriptors,
    std::array<std::int8_t, buckets::kDescriptorCapacity>& equipmentSlots) noexcept {
    equipmentSlots.fill(buckets::kUnavailableEquipmentSlot);
    std::uint32_t tableTag = 0;
    if (!find_bucket_definition_table(source, tableTag)) {
        return false;
    }
    std::uint32_t tableClass = 0;
    tables::Array table{};
    if (!reader::read_tag(source, storage.scratch, tableTag, storage.child, tableClass)) {
        report_bucket_equipment_failure("table_read", 0, 0);
        return false;
    }
    // The sweep and the reader resolve the installed entry independently, so the class the reader
    // reports still has to agree with the one the table was selected by.
    if (tableClass != tables::kBucketDefinitionTableClass) {
        report_bucket_equipment_failure("table_class", tableClass, 0);
        return false;
    }
    if (!tables::find_array_at(
            std::span<const std::byte>{storage.child}, tables::kTableArrayDescriptor, table)) {
        report_bucket_equipment_failure("table_array", storage.child.size(), 0);
        return false;
    }
    if (table.count != tables::kBucketDefinitionCount) {
        report_bucket_equipment_failure("table_count", table.count, table.elementClass);
        return false;
    }
    const std::span<const std::byte> tableBlob{storage.child};
    const std::size_t tableSize = table.count * tables::kBucketDefinitionSize;
    if (tableSize > tableBlob.size() || table.dataOffset > tableBlob.size() - tableSize) {
        report_bucket_equipment_failure("table_extent", table.dataOffset, tableBlob.size());
        return false;
    }
    std::array<bool, kEquipmentSlotCount> seenEquipmentSlots{};
    std::array<bool, buckets::kDescriptorCapacity> seenBuckets{};
    std::size_t mappedSlots = 0;
    for (std::size_t index = 0; index < table.count; ++index) {
        const std::size_t base = table.dataOffset + index * tables::kBucketDefinitionSize;
        const std::uint8_t equipmentSlot = std::to_integer<std::uint8_t>(
            tableBlob[base + tables::kBucketDefinitionEquipmentSlotOffset]);
        if (equipmentSlot != tables::kBucketDefinitionUnavailableEquipmentSlot
            && (equipmentSlot >= kEquipmentSlotCount || seenEquipmentSlots[equipmentSlot])) {
            report_bucket_equipment_failure("equipment_slot", index, equipmentSlot);
            return false;
        }
        if (equipmentSlot == tables::kBucketDefinitionUnavailableEquipmentSlot) {
            continue;
        }
        const std::uint8_t bucketId = kBucketByEquipmentSlot[equipmentSlot];
        const buckets::Descriptor* descriptor =
            bucketId < buckets::kDescriptorCapacity ? find_bucket(descriptors, bucketId) : nullptr;
        if (descriptor == nullptr || seenBuckets[bucketId]
            || descriptor->arraySelector != buckets::ArraySelector::character) {
            report_bucket_equipment_failure("bucket_relation", equipmentSlot, bucketId);
            return false;
        }
        seenEquipmentSlots[equipmentSlot] = true;
        seenBuckets[bucketId] = true;
        equipmentSlots[bucketId] = static_cast<std::int8_t>(equipmentSlot);
        ++mappedSlots;
    }
    if (mappedSlots != 18 || seenEquipmentSlots[16]) {
        report_bucket_equipment_failure("mapped_slots", mappedSlots, seenEquipmentSlots[16]);
        return false;
    }
    report_bucket_equipment_mapping(mappedSlots);
    return true;
}

} // namespace

/** Publishes the inventory bucket descriptors from the root's bucket table. */
bool build_buckets(const reader::Source& source,
                   Storage& storage,
                   std::span<const std::byte> root) noexcept {
    if (state::build_data::inventory_bucket_descriptors_ready()) {
        return true;
    }
    storage.bucketCount = 0;
    storage.equipmentSlotByBucket.fill(buckets::kUnavailableEquipmentSlot);
    std::uint32_t tableTag = 0;
    if (!tables::slot_tag(root, tables::kBucketTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
        return false;
    }
    const std::span<const std::byte> blob{storage.child};
    std::int32_t count = 0;
    if (blob.size() < tables::kBucketFirstDescriptor + sizeof count) {
        return false;
    }
    std::memcpy(&count, blob.data() + tables::kBucketCountOffset, sizeof count);
    if (count <= 0 || static_cast<std::size_t>(count) > buckets::kDescriptorCapacity) {
        return false;
    }
    const std::size_t bucketCount = static_cast<std::size_t>(count);
    std::array<buckets::Descriptor, buckets::kDescriptorCapacity> descriptors{};
    for (std::size_t index = 0; index < bucketCount; ++index) {
        const std::size_t base =
            tables::kBucketFirstDescriptor + index * tables::kBucketDescriptorSize;
        if (base + tables::kBucketDescriptorSize > blob.size()) {
            return false;
        }
        std::int32_t firstSlot = 0;
        std::int32_t slotCount = 0;
        std::memcpy(
            &firstSlot, blob.data() + base + tables::kBucketFirstSlotOffset, sizeof firstSlot);
        std::memcpy(
            &slotCount, blob.data() + base + tables::kBucketSlotCountOffset, sizeof slotCount);
        descriptors[index].bucketId = std::to_integer<std::uint8_t>(blob[base]);
        descriptors[index].firstSlot = static_cast<std::uint16_t>(firstSlot);
        descriptors[index].slotCount = static_cast<std::uint16_t>(slotCount);
        descriptors[index].arraySelector = static_cast<buckets::ArraySelector>(
            std::to_integer<std::uint8_t>(blob[base + tables::kBucketArraySelectorOffset]));
        const auto fifo = std::to_integer<std::uint8_t>(blob[base + tables::kBucketFifoOffset]);
        const auto noTransfer =
            std::to_integer<std::uint8_t>(blob[base + tables::kBucketNoTransferOnEvictionOffset]);
        if (fifo > 1 || noTransfer > 1) {
            return false;
        }
        descriptors[index].policyFlags =
            (fifo ? buckets::kFifo : 0) | (noTransfer ? buckets::kNoTransferOnEviction : 0);
    }
    std::array<std::int8_t, buckets::kDescriptorCapacity> equipmentSlots{};
    if (!read_bucket_equipment_slots(
            source, storage, std::span(descriptors).first(bucketCount), equipmentSlots)) {
        return false;
    }
    storage.bucketRows = descriptors;
    storage.bucketCount = bucketCount;
    storage.equipmentSlotByBucket = equipmentSlots;
    return true;
}

/** Publishes the socket entry list table from the root. */
bool build_socket_entry_lists(const reader::Source& source,
                              Storage& storage,
                              std::span<const std::byte> root) noexcept {
    namespace lists = state::build_data::socket_entry_lists;
    if (state::build_data::socket_entry_lists_ready()) {
        return true;
    }
    std::uint32_t tableTag = 0;
    tables::Array table{};
    if (!tables::slot_tag(root, tables::kSocketEntryListTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child)
        || !tables::find_array_at(
            std::span<const std::byte>{storage.child}, tables::kTableArrayDescriptor, table)
        || table.elementClass != tables::kSocketEntryListTableClass) {
        return false;
    }
    const std::span<const std::byte> blob{storage.child};
    std::vector<lists::Definition> rows(static_cast<std::size_t>(table.count));
    std::vector<lists::EntryTable> entryTables;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        lists::EntryTable entryTable{};
        bool carriesSuperLane = false;
        tables::IndexRow row{};
        if (!tables::index_row(blob, table, index, row)) {
            return false;
        }
        rows[index].definitionHash = row.definitionHash;
        rows[index].definitionIndex = static_cast<std::uint16_t>(index);
        if (row.targetTag == 0
            || !reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)) {
            continue;
        }
        const std::span<const std::byte> target{storage.definition};
        tables::Array entries{};
        if (!tables::find_array_at(target, tables::kSocketEntryArrayDescriptor, entries)) {
            continue;
        }
        rows[index].entryCount = static_cast<std::uint8_t>(entries.count);
        for (std::uint64_t entry = 0; entry < entries.count && entry < 64U; ++entry) {
            const std::size_t base =
                entries.dataOffset + static_cast<std::size_t>(entry) * tables::kSocketEntrySize;
            std::uint32_t plugSource = 0;
            if (base + tables::kSocketEntryKind + 1 > target.size()) {
                break;
            }
            std::memcpy(&plugSource,
                        target.data() + base + tables::kSocketEntryPlugSource,
                        sizeof plugSource);
            if (plugSource != tables::kNoPlugSource) {
                rows[index].readyMask |= std::uint64_t{1} << entry;
            }
            // The group and kind decide which entries the character's selection makes active.
            if (entry < lists::kEntryCapacity) {
                lists::Entry& record = entryTable.entries[static_cast<std::size_t>(entry)];
                record.plugSource = plugSource;
                std::memcpy(&record.group,
                            target.data() + base + tables::kSocketEntryGroup,
                            sizeof record.group);
                std::memcpy(&record.kind,
                            target.data() + base + tables::kSocketEntryKind,
                            sizeof record.kind);
                carriesSuperLane = carriesSuperLane || record.kind == lists::kSuperEntryKind;
            }
        }
        // Only a list with a super lane belongs to a subclass, and only those are selected.
        if (carriesSuperLane && entryTables.size() < lists::kEntryTableCapacity) {
            entryTable.definitionIndex = static_cast<std::uint16_t>(index);
            entryTables.push_back(entryTable);
        }
    }
    return state::build_data::publish_socket_entry_lists(rows, entryTables);
}

} // namespace sunrise::client::content::items::packages
