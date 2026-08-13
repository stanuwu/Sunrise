#include "loadout_resolver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "loadout_item_resolver.h"

namespace sunrise::middleware::datagen::family4::loadout {
namespace {

namespace authored_inventory = state::account::inventory;
namespace build_buckets = state::build_data::inventory::buckets;

/**
 * Records one runtime-derived semantic-to-native equipment-slot link.
 * @param semanticIndex Stable semantic slot array index.
 * @param nativeSlot Installed native equipment slot.
 * @param semanticToNative Shared cross-character semantic mapping.
 * @param nativeToSemantic Shared native collision map.
 * @return True when the relationship agrees with every previously resolved item.
 */
[[nodiscard]] bool record_equipment_slot(
    std::size_t semanticIndex,
    std::uint8_t nativeSlot,
    std::array<std::optional<std::uint8_t>, authored_inventory::kEquipmentSlotCount>&
        semanticToNative,
    std::array<std::optional<std::size_t>, state::build_data::items::details::kEquipmentSlotCount>&
        nativeToSemantic) noexcept {
    if (static_cast<std::size_t>(nativeSlot) >= nativeToSemantic.size()) {
        return false;
    }
    const std::optional<std::uint8_t>& existingNative = semanticToNative[semanticIndex];
    if (existingNative.has_value() && *existingNative != nativeSlot) {
        return false;
    }
    const std::optional<std::size_t>& existingSemantic = nativeToSemantic[nativeSlot];
    if (existingSemantic.has_value() && *existingSemantic != semanticIndex) {
        return false;
    }
    semanticToNative[semanticIndex] = nativeSlot;
    nativeToSemantic[nativeSlot] = semanticIndex;
    return true;
}

/**
 * Places one item into the lowest free row inside its runtime bucket range.
 * @param candidate Resolved item and validated character bucket.
 * @param occupied Shared physical-row occupancy map.
 * @param output Receives the placed item.
 * @return True when the bucket contains an unused row.
 */
[[nodiscard]] bool place_item(const Candidate& candidate,
                              std::array<bool, build_buckets::kCharacterSlotCapacity>& occupied,
                              ResolvedItem& output) noexcept {
    const std::size_t first = candidate.bucket.firstSlot;
    const std::size_t count = candidate.bucket.slotCount;
    if (first >= occupied.size() || count > occupied.size() - first) {
        return false;
    }
    for (std::size_t row = first; row < first + count; ++row) {
        if (occupied[row]) {
            continue;
        }
        occupied[row] = true;
        output = candidate.item;
        output.inventoryRow = static_cast<std::uint16_t>(row);
        return true;
    }
    return false;
}

} // namespace

/** Resolves the item instances one character owns, selected or not. */
bool resolve_instances(const state::AccountState& account,
                       std::size_t characterIndex,
                       ResolvedInstances& output) noexcept {
    output = {};
    if (account.characterCount > account.characters.size()
        || characterIndex >= account.characterCount || !state::build_data::item_definitions_ready()
        || !state::build_data::configured_item_details_ready()
        || !state::build_data::inventory_bucket_descriptors_ready()
        || !state::build_data::socket_entry_lists_ready()) {
        return false;
    }
    const std::size_t itemDefinitionCount = state::build_data::item_definition_count();
    const std::size_t socketEntryListCount = state::build_data::socket_entry_list_count();
    if (itemDefinitionCount == 0 || socketEntryListCount == 0) {
        return false;
    }

    std::array<ResolvedItem, kItemCapacity> resolved{};
    std::array<bool, build_buckets::kCharacterSlotCapacity> occupied{};
    std::size_t itemCount = 0;
    const state::CharacterState& character = account.characters[characterIndex];
    for (const std::optional<authored_inventory::Item>& authored : character.equipment.slots) {
        if (!authored.has_value()) {
            continue;
        }
        Candidate candidate{};
        if (itemCount >= resolved.size()
            || !resolve_item(
                *authored, character, itemDefinitionCount, socketEntryListCount, candidate)
            || !place_item(candidate, occupied, resolved[itemCount])) {
            return false;
        }
        resolved[itemCount].equipped = true;
        ++itemCount;
    }
    for (std::size_t inventoryIndex = 0;
         character.selected && inventoryIndex < character.inventory.itemCount; ++inventoryIndex) {
        Candidate candidate{};
        if (itemCount >= resolved.size()
            || !resolve_item(character.inventory.items[inventoryIndex], character, itemDefinitionCount, socketEntryListCount, candidate)
            || !place_item(candidate, occupied, resolved[itemCount])) return false;
        resolved[itemCount].equipped = false;
        ++itemCount;
    }
    std::sort(resolved.begin(),
              resolved.begin() + static_cast<std::ptrdiff_t>(itemCount),
              [](const ResolvedItem& first, const ResolvedItem& second) {
                  return first.inventoryRow < second.inventoryRow;
              });

    ResolvedInstances staged{};
    for (std::size_t index = 0; index < itemCount; ++index) {
        staged.items[index] = {resolved[index].equipmentSlot, resolved[index].instance};
    }
    staged.itemCount = itemCount;
    output = staged;
    return true;
}

/** Resolves one selected character's authored equipment through installed build data. */
bool resolve(const state::AccountState& account,
             std::size_t selectedCharacterIndex,
             ResolvedLoadout& output) noexcept {
    if (account.characterCount > account.characters.size()
        || selectedCharacterIndex >= account.characterCount
        || !account.characters[selectedCharacterIndex].selected
        || !state::build_data::item_definitions_ready()
        || !state::build_data::configured_item_details_ready()
        || !state::build_data::inventory_bucket_descriptors_ready()
        || !state::build_data::socket_entry_lists_ready()) {
        return false;
    }
    const std::size_t itemDefinitionCount = state::build_data::item_definition_count();
    const std::size_t socketEntryListCount = state::build_data::socket_entry_list_count();
    if (itemDefinitionCount == 0 || socketEntryListCount == 0) {
        return false;
    }

    std::array<std::optional<std::uint8_t>, authored_inventory::kEquipmentSlotCount>
        semanticToNative{};
    std::array<std::optional<std::size_t>, state::build_data::items::details::kEquipmentSlotCount>
        nativeToSemantic{};
    std::array<Candidate, kItemCapacity> selectedCandidates{};
    std::array<bool, kItemCapacity> selectedPresent{};
    std::array<std::uint64_t, state::kCharacterCapacity * kItemCapacity> instanceSoids{};
    std::size_t instanceSoidCount = 0;
    // Every character contributes to one stable semantic-to-native slot contract.
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        const state::CharacterState& character = account.characters[characterIndex];
        if (character.selected != (characterIndex == selectedCharacterIndex)) {
            return false;
        }
        for (std::size_t semanticIndex = 0; semanticIndex < character.equipment.slots.size();
             ++semanticIndex) {
            const std::optional<authored_inventory::Item>& authored =
                character.equipment.slots[semanticIndex];
            if (!authored.has_value()) {
                continue;
            }
            // Instance keys are account-wide identities even though only one loadout is encoded.
            const auto instanceSoidEnd =
                instanceSoids.cbegin() + static_cast<std::ptrdiff_t>(instanceSoidCount);
            if (std::find(instanceSoids.cbegin(), instanceSoidEnd, authored->instanceSoid)
                != instanceSoidEnd) {
                return false;
            }
            instanceSoids[instanceSoidCount++] = authored->instanceSoid;
            Candidate candidate{};
            if (!resolve_item(
                    *authored, character, itemDefinitionCount, socketEntryListCount, candidate)
                || !record_equipment_slot(semanticIndex,
                                          candidate.item.equipmentSlot,
                                          semanticToNative,
                                          nativeToSemantic)) {
                return false;
            }
            if (characterIndex == selectedCharacterIndex) {
                selectedCandidates[semanticIndex] = candidate;
                selectedCandidates[semanticIndex].item.equipped = true;
                selectedPresent[semanticIndex] = true;
            }
        }
        for (std::size_t inventoryIndex = 0; inventoryIndex < character.inventory.itemCount; ++inventoryIndex) {
            const authored_inventory::Item& authored = character.inventory.items[inventoryIndex];
            const auto instanceSoidEnd = instanceSoids.cbegin() + static_cast<std::ptrdiff_t>(instanceSoidCount);
            if (instanceSoidCount >= instanceSoids.size()
                || std::find(instanceSoids.cbegin(), instanceSoidEnd, authored.instanceSoid) != instanceSoidEnd) return false;
            instanceSoids[instanceSoidCount++] = authored.instanceSoid;
        }
    }

    ResolvedLoadout staged{};
    std::array<bool, build_buckets::kCharacterSlotCapacity> occupied{};
    for (std::size_t semanticIndex = 0; semanticIndex < selectedPresent.size(); ++semanticIndex) {
        if (!selectedPresent[semanticIndex]) {
            continue;
        }
        // Row placement is all or nothing; one full runtime bucket rejects the whole loadout.
        if (staged.itemCount >= staged.items.size()
            || !place_item(
                selectedCandidates[semanticIndex], occupied, staged.items[staged.itemCount])) {
            return false;
        }
        ++staged.itemCount;
    }
    const state::CharacterState& selectedCharacter = account.characters[selectedCharacterIndex];
    for (std::size_t inventoryIndex = 0; inventoryIndex < selectedCharacter.inventory.itemCount; ++inventoryIndex) {
        Candidate candidate{};
        if (staged.itemCount >= staged.items.size()
            || !resolve_item(selectedCharacter.inventory.items[inventoryIndex], selectedCharacter, itemDefinitionCount, socketEntryListCount, candidate)
            || !place_item(candidate, occupied, staged.items[staged.itemCount])) return false;
        staged.items[staged.itemCount].equipped = false;
        ++staged.itemCount;
    }
    std::sort(staged.items.begin(),
              staged.items.begin() + static_cast<std::ptrdiff_t>(staged.itemCount),
              [](const ResolvedItem& first, const ResolvedItem& second) {
                  return first.inventoryRow < second.inventoryRow;
              });
    for (std::size_t index = 0; index < staged.itemCount; ++index) {}
    staged.nextInventorySerial = static_cast<std::uint32_t>(staged.itemCount);

    // Counts are the publication-stability gate for the mappings resolved above.
    if (state::build_data::item_definition_count() != itemDefinitionCount
        || state::build_data::socket_entry_list_count() != socketEntryListCount) {
        return false;
    }
    output = staged;
    return true;
}

} // namespace sunrise::middleware::datagen::family4::loadout
