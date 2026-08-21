#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../middleware/content/packages/tables/unlock_expression.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

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



} // namespace

/**
 * Reads the presentation node table and resolves each node's value slot and owned records.
 *
 * A node's progress bar shows a value slot named by its own expression, and the records it owns sit
 * at row `+136` as a row and a gate. Both are read here so a claim never has to walk the node table.
 */
bool build_nodes(const reader::Source& source,
                 reader::Scratch& scratch,
                 std::span<const std::byte> root,
                 std::vector<std::byte>& blob,
                 std::span<state::build_data::nodes::Definition> output,
                 std::size_t& count) noexcept {
    namespace domain = state::build_data::nodes;
    count = 0;

    // The account flag mapping table, read first. A category's gate names a flag slot, and a slot
    // is not an index: the byte that feeds it sits at the row whose destination is that slot.
    std::uint32_t flagMapTag = 0;
    tables::Array flagMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> flagIndexBySlot{};
    if (tables::slot_tag(root, tables::kUnlockFlagMapTableSlot, flagMapTag) && flagMapTag != 0
        && tables::package_of(flagMapTag) != tables::kAbsentPackageId
        && reader::read_tag(source, scratch, flagMapTag, blob)
        && tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kAccountFlagMapDescriptor, flagMapRows)
        && flagMapRows.count != 0
        && flagMapRows.dataOffset
                   + static_cast<std::size_t>(flagMapRows.count) * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0; row < flagMapRows.count && row <= domain::kUnavailableFlagIndex;
             ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + flagMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            flagIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
    }


    tables::Array characterFlagMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> characterFlagIndexBySlot{};
    if (flagMapTag != 0
        && tables::find_array_at(std::span<const std::byte>{blob},
                                 tables::kCharacterFlagMapDescriptor,
                                 characterFlagMapRows)
        && characterFlagMapRows.count != 0
        && characterFlagMapRows.dataOffset
                   + static_cast<std::size_t>(characterFlagMapRows.count)
                         * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0;
             row < characterFlagMapRows.count && row <= domain::kUnavailableFlagIndex; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + characterFlagMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            characterFlagIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
    }

    std::uint32_t mapTag = 0;
    tables::Array mapRows{};
    if (!tables::slot_tag(root, tables::kUnlockValueMapTableSlot, mapTag) || mapTag == 0
        || tables::package_of(mapTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, mapTag, blob)
        || !tables::find_array_at(std::span<const std::byte>{blob},
                                  tables::kAccountValueMapDescriptor,
                                  mapRows)
        || mapRows.count == 0
        || mapRows.dataOffset
                   + static_cast<std::size_t>(mapRows.count) * tables::kUnlockMapRowStride
               > blob.size()) {
        report("value_map_fail", mapTag);
        return false;
    }
    std::unordered_map<std::int16_t, std::uint16_t> indexBySlot{};
    for (std::uint64_t row = 0; row < mapRows.count && row <= domain::kUnavailableValueIndex;
         ++row) {
        const std::size_t at =
            mapRows.dataOffset + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride;
        std::int16_t slot = 0;
        std::memcpy(&slot, blob.data() + at + tables::kUnlockMapDestinationSlotOffset, sizeof slot);
        indexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
    }


    tables::Array characterValueMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> characterValueIndexBySlot{};
    if (tables::find_array_at(std::span<const std::byte>{blob},
                              tables::kCharacterValueMapDescriptor,
                              characterValueMapRows)
        && characterValueMapRows.count != 0
        && characterValueMapRows.dataOffset
                   + static_cast<std::size_t>(characterValueMapRows.count)
                         * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0;
             row < characterValueMapRows.count && row <= domain::kUnavailableValueIndex; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + characterValueMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            characterValueIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
    }

    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kPresentationNodeTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, tableTag, blob)
        || !tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kTableArrayDescriptor, rows)
        || rows.count == 0 || rows.count > output.size()
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kNodeRowStride
               > blob.size()) {
        report("node_table_fail", tableTag);
        return false;
    }

    const std::span<const std::byte> table{blob};
    std::size_t driving = 0;
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kNodeRowStride;
        domain::Definition& definition = output[static_cast<std::size_t>(row)];
        definition = {};
        definition.definitionIndex = static_cast<std::uint16_t>(row);

        // The expression sits at one of two fields, and only one of them holds it on any node.
        std::int16_t slot = 0;
        const bool named =
            tables::expression_value_slot(table, at, tables::kNodeExpressionFieldPrimary, slot)
            || tables::expression_value_slot(table, at, tables::kNodeExpressionFieldAlternate, slot);
        if (named) {
            const auto found = indexBySlot.find(slot);
            if (found != indexBySlot.end()) {
                definition.valueIndex = found->second;
            }
            // The parent record's own bar reads the next slot up. Resolve it through the mapping
            // table rather than adding one to the index: rows happen to run in slot order around
            // here, but nothing guarantees that.
            const auto characterSlot = characterValueIndexBySlot.find(slot);
            if (characterSlot != characterValueIndexBySlot.end()) {
                definition.characterValueIndex = characterSlot->second;
            }
            const auto parent = indexBySlot.find(
                static_cast<std::int16_t>(slot + tables::kNodeParentSlotStep));
            if (parent != indexBySlot.end()) {
                definition.parentValueIndex = parent->second;
            }
        }

        // A category gated on a flag rather than on its own progress cannot reveal itself by being
        // played: with no title shown there is nothing inside to claim, and nothing to claim leaves
        // the gate shut. Resolve that flag so the gate can be satisfied.
        std::int16_t gateSlot = 0;
        if (tables::expression_flag_slot(table, at, tables::kNodeExpressionFieldPrimary, gateSlot)
            || tables::expression_flag_slot(table, at, tables::kNodeExpressionFieldAlternate, gateSlot)) {
            const auto gate = flagIndexBySlot.find(gateSlot);
            if (gate != flagIndexBySlot.end()) {
                definition.visibilityFlagIndex = gate->second;
            }
            const auto characterGate = characterFlagIndexBySlot.find(gateSlot);
            if (characterGate != characterFlagIndexBySlot.end()) {
                definition.visibilityCharacterFlagIndex = characterGate->second;
            }
        }



        // Records the node owns, four bytes each as a row and a gate.
        std::int64_t childCount = 0;
        std::int64_t childRelative = 0;
        std::memcpy(&childCount, table.data() + at + tables::kNodeChildRecordField,
                    sizeof childCount);
        std::memcpy(&childRelative, table.data() + at + tables::kNodeChildRecordField + 8,
                    sizeof childRelative);
        if (childCount >= 1 && childCount <= static_cast<std::int64_t>(domain::kChildCapacity)) {
            const std::size_t pointerAt = at + tables::kNodeChildRecordField + 8;
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
        if (definition.childCount != 0 && definition.valueIndex != domain::kUnavailableValueIndex) {
            ++driving;
        }
        ++count;
    }
    report("ok", static_cast<unsigned long long>(driving));
    return count != 0;
}

} // namespace sunrise::client::content::items::packages
