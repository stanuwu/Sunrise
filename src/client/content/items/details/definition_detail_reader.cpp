#include "definition_detail_reader.h"

#include <cstddef>
#include <cstdint>

#include "../../../../state/build_data/items/item_catalog.h"
#include "../layout.h"
#include "layout.h"
#include "memory.h"
#include "relative.h"
#include "socket_detail_reader.h"

namespace sunrise::client::content::items::details::reader {
namespace {

namespace build_details = state::build_data::items::details;
namespace dense_layout = sunrise::client::content::items::layout;

/**
 * Reads the optional signed equipment slot from one item definition.
 * @param source Installed item-detail source.
 * @param definition Loaded item definition address.
 * @param output Detail receiving the checked slot, if there is one.
 * @return True when the optional block is absent or carries slot 0 through 19.
 */
[[nodiscard]] bool read_equipment_slot(const investment::Source& source,
                                       std::uintptr_t definition,
                                       build_details::Definition& output) noexcept {
    bool present = false;
    std::uintptr_t block = 0;
    if (!relative::resolve_block(source,
                                 definition,
                                 offsetof(layout::ItemDefinition, equipmentBlockRelative),
                                 present,
                                 block)) {
        return false;
    }
    if (!present) {
        output.equipmentSlot.reset();
        return true;
    }

    std::int8_t slot = 0;
    if (!memory::read_member(source, block, offsetof(layout::EquipmentBlock, slot), slot)
        || slot < 0 || static_cast<std::size_t>(slot) >= build_details::kEquipmentSlotCount) {
        return false;
    }
    output.equipmentSlot = slot;
    return true;
}

} // namespace

/** Reads and checks one requested native item definition. */
bool read_definition(const investment::Source& source,
                     const Table& table,
                     std::uint16_t definitionIndex,
                     std::size_t socketEntryListRowCount,
                     build_details::Definition& output) noexcept {
    std::uintptr_t rowOffset = 0;
    std::uintptr_t rowAddress = 0;
    dense_layout::ItemIndexRow row{};
    if (!memory::multiply_fits(definitionIndex, sizeof(dense_layout::ItemIndexRow), rowOffset)
        || !memory::add_fits(table.rowsAddress, rowOffset, rowAddress)
        || !memory::read_value(source, rowAddress, row)) {
        return false;
    }

    std::uintptr_t target = 0;
    if (!handles::resolve(source.handles, row.targetHandle, target)) {
        return false;
    }

    build_details::Definition candidate{};
    candidate.definitionIndex = definitionIndex;
    std::uint8_t instancedDefinitionPredicate = 0;
    if (!memory::read_member(
            source, target, offsetof(layout::ItemDefinition, maxStackSize), candidate.maxStackSize)
        || !memory::read_member(
            source, target, offsetof(layout::ItemDefinition, bucketId), candidate.bucketId)
        || !memory::read_member(source,
                                target,
                                offsetof(layout::ItemDefinition, instancedDefinitionPredicate),
                                instancedDefinitionPredicate)
        || candidate.maxStackSize <= 0
        || candidate.bucketId == state::build_data::items::kUnresolvedBucketId
        || !read_equipment_slot(source, target, candidate)
        || !socket_reader::read_ordinary(source, target, table.rowCount, candidate)
        || !socket_reader::read_list_index(source, target, socketEntryListRowCount, candidate)) {
        return false;
    }
    candidate.instancedDefinitionState = instancedDefinitionPredicate == 0
                                             ? build_details::InstancedDefinitionState::stackable
                                             : build_details::InstancedDefinitionState::instanced;

    output = candidate;
    return true;
}

} // namespace sunrise::client::content::items::details::reader
