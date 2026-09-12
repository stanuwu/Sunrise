#include <array>
#include <cstdio>
#include <cstring>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/unlock_expression.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::nodes;
namespace record_domain = state::build_data::records;

/** Reports where the node pass stopped, so a silent miss cannot look like a stuck progress bar. */
void report(const char* stage, unsigned long long detail) noexcept {
    std::array<char, 128> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=pkg stage=nodes result=%s detail=%llu", stage, detail);
    if (count > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Clears one map so every slot reads as unmapped. */
void clear_slot_map(SlotMap& output) noexcept {
    output.fill(kUnmappedSlot);
}

/** Clears all four so an unread table cannot leave a prior pass's indexes addressable. */
void clear_slot_maps(SlotMaps& maps) noexcept {
    clear_slot_map(maps.accountFlag);
    clear_slot_map(maps.characterFlag);
    clear_slot_map(maps.accountValue);
    clear_slot_map(maps.characterValue);
}

/**
 * Reads one unlock mapping table into a slot-to-index map.
 * @param blob Blob holding the mapping table.
 * @param descriptor Array descriptor of the mapping table inside that blob.
 * @param output Receives one bank index per addressable slot, or the unavailable index.
 * @return True when the table resolves and every row fits the blob.
 */
[[nodiscard]] bool
read_slot_map(std::span<const std::byte> blob, std::size_t descriptor, SlotMap& output) noexcept {
    clear_slot_map(output);
    tables::Array rows{};
    if (!tables::find_array_at(blob, descriptor, rows) || rows.count == 0
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kUnlockMapRowStride
               > blob.size()) {
        return false;
    }
    for (std::uint64_t row = 0; row < rows.count && row < kUnmappedSlot; ++row) {
        std::int16_t slot = 0;
        std::memcpy(&slot,
                    blob.data() + rows.dataOffset
                        + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                        + tables::kUnlockMapDestinationSlotOffset,
                    sizeof slot);
        // An expression operand is never negative, so a negative destination addresses nothing.
        if (slot < 0) {
            continue;
        }
        std::uint16_t& existing = output[static_cast<std::size_t>(slot)];
        if (existing == kUnmappedSlot) {
            existing = static_cast<std::uint16_t>(row);
        }
    }
    return true;
}

/**
 * Resolves one node's lore-book flag and the value slot its parent bar reads.
 * A book's parent triumph is the one child record that displays no lore. Its first objective
 * names the slot the book's collected-chapter bar counts in.
 * @param storage Pass storage holding the record rows and the objective bank.
 * @param node Node whose children were already read.
 * @param parentSlot Receives the parent bar's raw value slot, or -1 when the book has none.
 * @return True when at least one child record displays lore, which is what makes a book.
 */
[[nodiscard]] bool resolve_book(const Storage& storage,
                                const domain::Definition& node,
                                std::int16_t& parentSlot) noexcept {
    parentSlot = -1;
    bool book = false;
    for (std::size_t child = 0; child < node.childCount; ++child) {
        const std::size_t row = node.children[child];
        if (row >= storage.recordCount) {
            continue;
        }
        const record_domain::Definition& record = storage.recordRows[row];
        if (record.loreRow != record_domain::kUnavailableLoreRow) {
            book = true;
            continue;
        }
        for (std::size_t entry = 0; entry < record.objectiveCount; ++entry) {
            const std::size_t at = static_cast<std::size_t>(record.objectiveOffset) + entry;
            if (at < storage.recordObjectiveCount
                && storage.recordObjectives[at].sourceValueSlot >= 0) {
                parentSlot = storage.recordObjectives[at].sourceValueSlot;
            }
        }
    }
    return book;
}

} // namespace

/**
 * A gate names a slot; its saved bank index is the mapping row that names that slot.
 * @param source Borrowed package source.
 * @param storage Receives four slot maps and retained value-map bytes; may be partial on failure.
 * @param root Investment root bytes naming the flag and value mapping tables.
 * @return True when both account maps read; a character map may remain unmapped.
 */
bool read_unlock_slot_maps(const reader::Source& source,
                           Storage& storage,
                           std::span<const std::byte> root) noexcept {
    SlotMaps& maps = storage.slotMaps;
    clear_slot_maps(maps);
    std::uint32_t flagMapTag = 0;
    if (!tables::slot_tag(root, tables::kUnlockFlagMapTableSlot, flagMapTag) || flagMapTag == 0
        || tables::package_of(flagMapTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, storage.scratch, flagMapTag, storage.child)) {
        return false;
    }
    const std::span<const std::byte> flagMap{storage.child};
    const bool flagMapRead =
        read_slot_map(flagMap, tables::kAccountFlagMapDescriptor, maps.accountFlag);
    (void)read_slot_map(flagMap, tables::kCharacterFlagMapDescriptor, maps.characterFlag);

    std::uint32_t valueMapTag = 0;
    if (!flagMapRead || !tables::slot_tag(root, tables::kUnlockValueMapTableSlot, valueMapTag)
        || valueMapTag == 0 || tables::package_of(valueMapTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, storage.scratch, valueMapTag, storage.child)) {
        return false;
    }
    const std::span<const std::byte> valueMap{storage.child};
    storage.questValueMap = storage.child;
    const bool valueMapRead =
        read_slot_map(valueMap, tables::kAccountValueMapDescriptor, maps.accountValue);
    (void)read_slot_map(valueMap, tables::kCharacterValueMapDescriptor, maps.characterValue);
    return valueMapRead;
}

std::uint16_t bank_index(const SlotMap& map, std::int32_t slot) noexcept {
    if (slot < 0 || static_cast<std::size_t>(slot) >= map.size()) {
        return kUnmappedSlot;
    }
    return map[static_cast<std::size_t>(slot)];
}

/** Reads nodes and resolves their value slots, owned records and lore parent bars. */
bool build_nodes(const reader::Source& source,
                 Storage& storage,
                 std::span<const std::byte> root) noexcept {
    storage.nodeCount = 0;

    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kPresentationNodeTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child)
        || !tables::find_array_at(
            std::span<const std::byte>{storage.child}, tables::kTableArrayDescriptor, rows)
        || rows.count == 0 || rows.count > storage.nodeRows.size()
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kNodeRowStride
               > storage.child.size()) {
        report("node_table_fail", tableTag);
        return false;
    }

    const std::span<const std::byte> table{storage.child};
    std::size_t books = 0;

    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kNodeRowStride;
        domain::Definition& definition = storage.nodeRows[static_cast<std::size_t>(row)];
        definition = {};
        definition.definitionIndex = static_cast<std::uint16_t>(row);

        // The expression sits at one of two fields, and only one of them holds it on any node.
        std::int16_t slot = 0;
        const bool named =
            tables::expression_value_slot(table, at, tables::kNodeExpressionFieldPrimary, slot)
            || tables::expression_value_slot(
                table, at, tables::kNodeExpressionFieldAlternate, slot);
        if (named) {
            definition.valueSlot = slot;
            definition.valueIndex = bank_index(storage.slotMaps.accountValue, slot);
            // One book's bar reads a slot only the character table carries, so both scopes resolve.
            definition.characterValueSlot = slot;
            definition.characterValueIndex = bank_index(storage.slotMaps.characterValue, slot);
        }

        // A category gated on a flag never opens from progress alone, so resolve that flag.
        std::int16_t gateSlot = 0;
        if (tables::expression_flag_slot(table, at, tables::kNodeExpressionFieldPrimary, gateSlot)
            || tables::expression_flag_slot(
                table, at, tables::kNodeExpressionFieldAlternate, gateSlot)) {
            definition.visibilityFlagIndex = bank_index(storage.slotMaps.accountFlag, gateSlot);
            definition.visibilityCharacterFlagIndex =
                bank_index(storage.slotMaps.characterFlag, gateSlot);
        }

        // Records the node owns, four bytes each as a row and a gate.
        std::int64_t childCount = 0;
        std::int64_t childRelative = 0;
        const std::size_t pointerAt =
            at + tables::kNodeChildRecordField + tables::kUnlockExpressionPointerOffset;
        std::memcpy(
            &childCount, table.data() + at + tables::kNodeChildRecordField, sizeof childCount);
        std::memcpy(&childRelative, table.data() + pointerAt, sizeof childRelative);
        if (childCount >= 1 && childCount <= static_cast<std::int64_t>(domain::kChildCapacity)) {
            const std::int64_t target = static_cast<std::int64_t>(pointerAt) + childRelative
                                        + static_cast<std::int64_t>(tables::kHeaderSkip);
            if (target >= 0
                && static_cast<std::size_t>(target)
                           + static_cast<std::size_t>(childCount) * tables::kNodeChildRecordStride
                       <= table.size()) {
                const auto base = static_cast<std::size_t>(target);
                for (std::int64_t index = 0; index < childCount; ++index) {
                    std::uint16_t childRow = 0;
                    std::memcpy(&childRow,
                                table.data() + base
                                    + static_cast<std::size_t>(index)
                                          * tables::kNodeChildRecordStride,
                                sizeof childRow);
                    definition.children[static_cast<std::size_t>(definition.childCount++)] =
                        childRow;
                }
            }
        }

        // A lore book and its parent bar are read from the records the node owns, never from the
        // order the value bank happens to allocate its slots in.
        std::int16_t parentSlot = -1;
        definition.loreBook = resolve_book(storage, definition, parentSlot);
        if (definition.loreBook && parentSlot >= 0) {
            definition.parentValueIndex = bank_index(storage.slotMaps.accountValue, parentSlot);
            definition.parentCharacterValueIndex =
                bank_index(storage.slotMaps.characterValue, parentSlot);
        }
        books += definition.loreBook ? 1U : 0U;
        ++storage.nodeCount;
    }

    report("ok", static_cast<unsigned long long>(books));
    return storage.nodeCount != 0;
}

} // namespace sunrise::client::content::items::packages
