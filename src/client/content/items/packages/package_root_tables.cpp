#include <cstring>
#include <vector>

#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/socket_entry_lists/definition.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {

/** Publishes the inventory bucket descriptors from the root's bucket table. */
bool build_buckets(const reader::Source& source,
                   Storage& storage,
                   std::span<const std::byte> root) noexcept {
    namespace buckets = state::build_data::inventory::buckets;
    if (state::build_data::inventory_bucket_descriptors_ready()) {
        return true;
    }
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
    std::vector<buckets::Descriptor> rows(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < rows.size(); ++index) {
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
        rows[index].bucketId = std::to_integer<std::uint8_t>(blob[base]);
        rows[index].firstSlot = static_cast<std::uint16_t>(firstSlot);
        rows[index].slotCount = static_cast<std::uint16_t>(slotCount);
        rows[index].arraySelector = static_cast<buckets::ArraySelector>(
            std::to_integer<std::uint8_t>(blob[base + tables::kBucketArraySelectorOffset]));
    }
    return state::build_data::publish_inventory_bucket_descriptors(rows);
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
