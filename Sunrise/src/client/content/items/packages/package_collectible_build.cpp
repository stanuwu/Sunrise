#include <array>
#include <cstring>
#include <limits>

#include "../../../../middleware/content/packages/schema/generated/tag_classes_80807a.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80807c.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808087.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/sobjects/sobject_catalog.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/**
 * Reads the one flag slot a collectible's acquired-state expression tests.
 * A collectible gated by anything other than one plain flag test keeps the unavailable slot.
 * @param table Collectible table bytes.
 * @param at Collectible row offset.
 * @param slot Receives the tested slot, or the unavailable slot.
 */
void read_acquired_flag_slot(std::span<const std::byte> table,
                             std::size_t at,
                             std::uint16_t& slot) noexcept {
    namespace domain = state::build_data::collectibles;
    slot = domain::kUnavailableFlagSlot;
    tables::Array expression{};
    std::uint32_t opcode = 0;
    std::uint32_t operand = 0;
    if (!tables::find_optional_array_at(
            table, at + tables::kCollectibleAcquiredExpressionField, expression)
        || expression.count != 1 || expression.elementClass != tables::kInvestmentExpressionRowClass
        || expression.dataOffset + tables::kUnlockInstructionStride > table.size()) {
        return;
    }
    std::memcpy(&opcode,
                table.data() + expression.dataOffset + tables::kUnlockInstructionOpcodeOffset,
                sizeof opcode);
    std::memcpy(&operand,
                table.data() + expression.dataOffset + tables::kUnlockInstructionOperandOffset,
                sizeof operand);
    if (opcode == tables::kUnlockReadFlagOpcode && operand < domain::kUnavailableFlagSlot) {
        slot = static_cast<std::uint16_t>(operand);
    }
}

/**
 * Resolves one unlock flag slot to the bank row it feeds.
 * @param table Unlock flag slot table bytes.
 * @param rows Located slot array, which is keyed by slot.
 * @param slot Slot to resolve.
 * @return The bank row, or the unavailable row when the slot is not in the table.
 */
[[nodiscard]] std::uint16_t flag_bank_index(std::span<const std::byte> table,
                                            const tables::Array& rows,
                                            std::uint16_t slot) noexcept {
    namespace domain = state::build_data::collectibles;
    const std::size_t at = rows.dataOffset
                           + static_cast<std::size_t>(slot) * tables::kUnlockSlotRowStride
                           + tables::kUnlockSlotBankIndexOffset;
    std::uint16_t index = domain::kUnavailableFlagIndex;
    if (slot >= rows.count || at + sizeof index > table.size()) {
        return domain::kUnavailableFlagIndex;
    }
    std::memcpy(&index, table.data() + at, sizeof index);
    return index;
}

} // namespace

/** Reads the fixed collectible table and publishes its native-index item links. */
bool build_collectibles(const reader::Source& source,
                        Storage& storage,
                        std::span<const std::byte> root,
                        std::uint64_t itemDefinitionCount) noexcept {
    namespace domain = state::build_data::collectibles;

    // A collectible picked up in the world arrives as an incident, and its target is a row of the
    // sobject table. Without that table the target is a bare number.
    if (!state::build_data::sobject_definitions_ready()) {
        namespace sobjects = state::build_data::sobjects;
        for (const std::uint32_t tag :
             {tables::kSobjectTablePrimaryTag, tables::kSobjectTableAlternateTag}) {
            std::vector<std::byte>& blob = storage.definition;
            tables::Array sobjectRows{};
            if (!reader::read_tag(source, storage.scratch, tag, blob)
                || !tables::read_sobject_rows(std::span<const std::byte>{blob}, sobjectRows)
                || sobjectRows.count == 0 || sobjectRows.count > sobjects::kDefinitionCapacity) {
                continue;
            }
            const auto count = static_cast<std::size_t>(sobjectRows.count);
            for (std::size_t row = 0; row < count; ++row) {
                const std::size_t at = sobjectRows.dataOffset + row * tables::kSobjectRowStride;
                sobjects::Definition& definition = storage.sobjectRows[row];
                std::memcpy(&definition.nameHash,
                            blob.data() + at + tables::kSobjectNameHashOffset,
                            sizeof definition.nameHash);
                std::memcpy(&definition.lane4,
                            blob.data() + at + tables::kSobjectLaneOffset,
                            sizeof definition.lane4);
                definition.typeCode = tables::sobject_type_code(blob, at);
            }
            (void)state::build_data::publish_sobject_definitions(
                std::span(storage.sobjectRows).first(count));
            break;
        }
    }

    if (state::build_data::collectible_definitions_ready()) {
        return true;
    }
    if (itemDefinitionCount == 0
        || itemDefinitionCount > state::build_data::items::kDefinitionCapacity) {
        return false;
    }

    std::uint32_t requirementTableTag = 0;
    std::uint32_t requirementTableClass = 0;
    tables::Array requirementSets{};
    if (!tables::slot_tag(root, tables::kMaterialRequirementTableSlot, requirementTableTag)
        || requirementTableTag == 0
        || tables::package_of(requirementTableTag) == tables::kAbsentPackageId
        || !reader::read_tag(
            source, storage.scratch, requirementTableTag, storage.definition, requirementTableClass)
        || requirementTableClass != tables::kMaterialRequirementTableClass
        || !tables::find_array_at(std::span<const std::byte>{storage.definition},
                                  offsetof(generated::MaterialRequirementTable, table),
                                  requirementSets)
        || requirementSets.elementClass != tables::kMaterialRequirementSetRowClass
        || requirementSets.count == 0
        || requirementSets.count > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const std::span<const std::byte> requirementTable{storage.definition};
    if (requirementSets.dataOffset > requirementTable.size()
        || requirementSets.count > (requirementTable.size() - requirementSets.dataOffset)
                                       / tables::kMaterialRequirementSetRowStride) {
        return false;
    }

    // The slot table is keyed by slot, so it resolves each collectible's acquired flag to the
    // bank row the object its kind names carries.
    std::uint32_t slotTableTag = 0;
    tables::Array slotRows{};
    if (!tables::slot_tag(root, tables::kUnlockFlagSlotTableSlot, slotTableTag) || slotTableTag == 0
        || !reader::read_tag(source, storage.scratch, slotTableTag, storage.unlockSlotTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.unlockSlotTable},
                                  offsetof(generated::UnlockFlagTable, table),
                                  slotRows)) {
        return false;
    }
    const std::span<const std::byte> slotTable{storage.unlockSlotTable};

    std::uint32_t tableTag = 0;
    std::uint32_t tableClass = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kCollectibleTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child, tableClass)
        || tableClass != tables::kCollectibleTableClass
        || !tables::find_array_at(std::span<const std::byte>{storage.child},
                                  offsetof(generated::CollectibleTable, table),
                                  rows)
        || rows.elementClass != tables::kCollectibleRowClass || rows.count == 0
        || rows.count > storage.collectibleRows.size()) {
        return false;
    }

    const std::span<const std::byte> table{storage.child};
    if (rows.dataOffset > table.size()
        || rows.count > (table.size() - rows.dataOffset) / tables::kCollectibleRowStride) {
        return false;
    }
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kCollectibleRowStride;
        std::uint32_t collectibleHash = 0;
        std::uint16_t itemDefinitionIndex = domain::kUnavailableItemDefinitionIndex;
        std::uint16_t requirementSetIndex = domain::kUnavailableMaterialRequirementSetIndex;
        std::memcpy(&collectibleHash,
                    table.data() + at + tables::kCollectibleHashOffset,
                    sizeof collectibleHash);
        std::memcpy(&itemDefinitionIndex,
                    table.data() + at + tables::kCollectibleItemIndexOffset,
                    sizeof itemDefinitionIndex);
        std::memcpy(&requirementSetIndex,
                    table.data() + at + tables::kCollectibleMaterialRequirementIndexOffset,
                    sizeof requirementSetIndex);
        if (itemDefinitionIndex != domain::kUnavailableItemDefinitionIndex
            && itemDefinitionIndex >= itemDefinitionCount) {
            return false;
        }
        domain::Definition& output = storage.collectibleRows[static_cast<std::size_t>(row)];
        output = {};
        output.collectibleHash = collectibleHash;
        output.collectibleIndex = static_cast<std::uint16_t>(row);
        output.itemDefinitionIndex = itemDefinitionIndex;
        read_acquired_flag_slot(table, at, output.acquiredFlagSlot);
        if (output.acquiredFlagSlot != domain::kUnavailableFlagSlot) {
            output.acquiredFlagIndex =
                flag_bank_index(slotTable, slotRows, output.acquiredFlagSlot);
        }
        if (requirementSetIndex == domain::kUnavailableMaterialRequirementSetIndex) {
            continue;
        }
        if (requirementSetIndex >= requirementSets.count) {
            return false;
        }
        const std::size_t setAt = requirementSets.dataOffset
                                  + static_cast<std::size_t>(requirementSetIndex)
                                        * tables::kMaterialRequirementSetRowStride;
        std::uint32_t requirementSetHash = 0;
        std::int64_t descriptorRelative = 0;
        std::memcpy(&requirementSetHash,
                    requirementTable.data() + setAt + tables::kMaterialRequirementSetHashOffset,
                    sizeof requirementSetHash);
        std::memcpy(&descriptorRelative,
                    requirementTable.data() + setAt
                        + tables::kMaterialRequirementSetArrayPointerOffset,
                    sizeof descriptorRelative);
        const std::size_t pointerAt = setAt + tables::kMaterialRequirementSetArrayPointerOffset;
        if (requirementSetHash == 0 || descriptorRelative < -static_cast<std::int64_t>(pointerAt)
            || descriptorRelative
                   > static_cast<std::int64_t>(requirementTable.size() - pointerAt)) {
            return false;
        }
        const auto descriptorSigned = static_cast<std::int64_t>(pointerAt) + descriptorRelative;
        if (descriptorSigned < 0) {
            return false;
        }
        tables::Array requirements{};
        if (!tables::find_array_at(
                requirementTable, static_cast<std::size_t>(descriptorSigned), requirements)
            || requirements.elementClass != tables::kMaterialRequirementRowClass
            || requirements.count == 0 || requirements.count > output.materialRequirements.size()
            || requirements.dataOffset > requirementTable.size()
            || requirements.count > (requirementTable.size() - requirements.dataOffset)
                                        / tables::kMaterialRequirementRowStride) {
            return false;
        }
        output.materialRequirementSetHash = requirementSetHash;
        output.materialRequirementSetIndex = requirementSetIndex;
        output.materialRequirementCount = static_cast<std::uint8_t>(requirements.count);
        for (std::size_t requirement = 0; requirement < requirements.count; ++requirement) {
            const std::size_t requirementAt =
                requirements.dataOffset + requirement * tables::kMaterialRequirementRowStride;
            std::uint16_t nativeItemIndex = 0;
            std::uint32_t quantity = 0;
            std::uint8_t deleteOnAction = 0;
            std::uint8_t omitFromRequirements = 0;
            std::uint16_t sentinel = 0;
            std::memcpy(&nativeItemIndex,
                        requirementTable.data() + requirementAt
                            + tables::kMaterialRequirementItemIndexOffset,
                        sizeof nativeItemIndex);
            std::memcpy(&quantity,
                        requirementTable.data() + requirementAt
                            + tables::kMaterialRequirementQuantityOffset,
                        sizeof quantity);
            std::memcpy(&deleteOnAction,
                        requirementTable.data() + requirementAt
                            + tables::kMaterialRequirementDeleteOffset,
                        sizeof deleteOnAction);
            std::memcpy(&omitFromRequirements,
                        requirementTable.data() + requirementAt
                            + tables::kMaterialRequirementOmitOffset,
                        sizeof omitFromRequirements);
            std::memcpy(&sentinel,
                        requirementTable.data() + requirementAt
                            + tables::kMaterialRequirementSentinelOffset,
                        sizeof sentinel);
            if (nativeItemIndex >= itemDefinitionCount || quantity == 0 || deleteOnAction > 1
                || omitFromRequirements > 1 || sentinel != 0xFFFFU) {
                return false;
            }
            output.materialRequirements[requirement] = {
                quantity,
                nativeItemIndex,
                deleteOnAction != 0,
                omitFromRequirements != 0,
            };
        }
    }
    return state::build_data::publish_collectible_definitions(
        std::span(storage.collectibleRows).first(static_cast<std::size_t>(rows.count)));
}

} // namespace sunrise::client::content::items::packages
