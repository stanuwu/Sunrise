#include "inspection_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace sunrise::middleware::datagen::inspection {

bool encode_root(std::uint64_t accountSoid,
                 std::uint64_t characterSoid,
                 std::span<std::byte> output) noexcept {
    if (accountSoid == 0 || characterSoid == 0 || output.size() < kRootSize) {
        return false;
    }
    Root object{};
    object.accountSoid = accountSoid;
    object.characterSoid = characterSoid;
    // Inspection is a public projection. Private progression banks have no producer here.
    for (auto& entry : object.progressions) {
        entry.definitionIndex = character_record::layout::kEmptyDefinitionIndex;
    }
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

bool equipped_instances(const family4::loadout::ResolvedLoadout& loadout,
                        family4::loadout::ResolvedInstances& output) noexcept {
    if (loadout.itemCount > loadout.items.size()) {
        return false;
    }
    family4::loadout::ResolvedInstances staged{};
    std::array<bool, kEquipmentCapacity> slots{};
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const auto& item = loadout.items[index];
        if (!item.equipped) {
            continue;
        }
        if (item.equipmentSlot >= slots.size() || slots[item.equipmentSlot]
            || item.instance.instanceSoid == 0 || staged.itemCount == kEquipmentCapacity) {
            return false;
        }
        for (std::size_t prior = 0; prior < staged.itemCount; ++prior) {
            if (staged.items[prior].instance.instanceSoid == item.instance.instanceSoid) {
                return false;
            }
        }
        slots[item.equipmentSlot] = true;
        staged.items[staged.itemCount++] = {item.equipmentSlot, item.instance};
    }
    output = staged;
    return true;
}

bool encode_character(const state::CharacterState& character,
                      const family4::loadout::ResolvedLoadout& loadout,
                      const family4::loadout::ResolvedInstances& equipped,
                      std::int32_t light,
                      std::span<std::byte> output) noexcept {
    if (output.size() < kCharacterSize || loadout.itemCount > loadout.items.size()
        || equipped.itemCount > kEquipmentCapacity
        || loadout.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    // All three blocks below are the identical native schema classes, not inferred offset twins.
    std::array<std::byte, character_record::kFamily0RecordSize> banner{};
    if (!character_record::encode_family0(character, equipped, light, banner)) {
        return false;
    }
    Character object{};
    constexpr std::size_t appearanceOffset = character_record::layout::kIdentitySize;
    constexpr std::size_t summaryOffset =
        appearanceOffset + character_record::layout::kAppearanceSize;
    std::memcpy(&object.identity, banner.data(), sizeof object.identity);
    std::memcpy(&object.appearance, banner.data() + appearanceOffset, sizeof object.appearance);
    std::memcpy(&object.summary, banner.data() + summaryOffset, sizeof object.summary);
    for (auto& entry : object.progressions) {
        entry.definitionIndex = character_record::layout::kEmptyDefinitionIndex;
    }
    object.inventorySerial = static_cast<std::int32_t>(loadout.nextInventorySerial);
    for (auto& row : object.inventory) {
        row.definitionIndex = character_record::layout::kEmptyDefinitionIndex;
    }
    std::array<bool, kEquipmentCapacity> slots{};
    std::size_t equippedCount = 0;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const auto& item = loadout.items[index];
        if (!item.equipped) {
            continue;
        }
        if (item.equipmentSlot >= slots.size() || slots[item.equipmentSlot]
            || equippedCount >= equipped.itemCount
            || equipped.items[equippedCount].equipmentSlot != item.equipmentSlot
            || equipped.items[equippedCount].instance.instanceSoid != item.instance.instanceSoid
            || item.quantity <= 0 || item.mutationSerial < 0
            || static_cast<std::uint32_t>(item.mutationSerial) >= loadout.nextInventorySerial) {
            return false;
        }
        ++equippedCount;
        slots[item.equipmentSlot] = true;
        auto& row = object.inventory[item.equipmentSlot];
        row.definitionIndex = item.instance.baseDefinitionIndex;
        row.instanceSoid = item.instance.instanceSoid;
        row.quantity = item.quantity;
        row.mutationSerial = item.mutationSerial;
        row.flags = item.flags;
        object.equippedSoids[item.equipmentSlot] = item.instance.instanceSoid;
    }
    if (equippedCount != equipped.itemCount) {
        return false;
    }
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::inspection
