/**
 * Equipment placement helpers: native and semantic slots, resolved positions, and the
 * comparisons one equipment transition is checked against.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Resolves the installed native equipment slot for one configured authored item. */
[[nodiscard]] bool native_equipment_slot(const authored_inventory::Item& item,
                                         std::uint8_t& slot) noexcept {
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || definition.definitionHash != item.definitionHash
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId || !detail.equipmentSlot.has_value()
        || *detail.equipmentSlot < 0
        || static_cast<std::size_t>(*detail.equipmentSlot) >= item_details::kEquipmentSlotCount) {
        return false;
    }
    slot = static_cast<std::uint8_t>(*detail.equipmentSlot);
    return true;
}

/** Resolves the installed physical inventory bucket for one configured authored item. */
[[nodiscard]] bool inventory_bucket_id(const authored_inventory::Item& item,
                                       std::uint8_t& bucketId) noexcept {
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || definition.definitionHash != item.definitionHash
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId) {
        return false;
    }
    bucketId = detail.bucketId;
    return true;
}

/** Maps the 16 proven native equipment positions onto their stable authored State slots. */
[[nodiscard]] bool semantic_equipment_slot(std::uint8_t nativeSlot,
                                           std::size_t& semanticIndex) noexcept {
    using EquipmentSlot = authored_inventory::EquipmentSlot;
    EquipmentSlot semanticSlot = EquipmentSlot::count;
    switch (nativeSlot) {
    case 0:
        semanticSlot = EquipmentSlot::subclass;
        break;
    case 1:
        semanticSlot = EquipmentSlot::helmet;
        break;
    case 2:
        semanticSlot = EquipmentSlot::gauntlets;
        break;
    case 4:
        semanticSlot = EquipmentSlot::chest;
        break;
    case 5:
        semanticSlot = EquipmentSlot::legs;
        break;
    case 6:
        semanticSlot = EquipmentSlot::classItem;
        break;
    case 7:
        semanticSlot = EquipmentSlot::kinetic;
        break;
    case 8:
        semanticSlot = EquipmentSlot::energy;
        break;
    case 9:
        semanticSlot = EquipmentSlot::heavy;
        break;
    case 10:
        semanticSlot = EquipmentSlot::ship;
        break;
    case 11:
        semanticSlot = EquipmentSlot::vehicle;
        break;
    case 12:
        semanticSlot = EquipmentSlot::ghost;
        break;
    case 13:
        semanticSlot = EquipmentSlot::emblem;
        break;
    case 14:
        semanticSlot = EquipmentSlot::emote;
        break;
    case 15:
        semanticSlot = EquipmentSlot::clanBanner;
        break;
    case 17:
        semanticSlot = EquipmentSlot::finisher;
        break;
    default:
        return false;
    }
    semanticIndex = static_cast<std::size_t>(semanticSlot);
    return semanticIndex < authored_inventory::kEquipmentSlotCount;
}

/** Finds one instance exactly once in a checked, row-sorted loadout. */
[[nodiscard]] bool find_resolved_position(const family4_loadout::ResolvedLoadout& loadout,
                                          std::uint64_t instanceSoid,
                                          ResolvedPosition& position) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        position.inventoryRow = item.inventoryRow;
        position.equipmentSlot = item.equipmentSlot;
        position.equipped = item.equipped;
        position.mutationSerial = item.mutationSerial;
    }
    return found;
}

/** @return True when the native placement and equipped marker are unchanged. */
[[nodiscard]] bool same_position(const ResolvedPosition& left,
                                 const ResolvedPosition& right) noexcept {
    return left.inventoryRow == right.inventoryRow && left.equipmentSlot == right.equipmentSlot
           && left.equipped == right.equipped;
}

/**
 * Applies canonical mutation generations after one shape-only equipment transition.
 * Every surviving instance keeps its native bucket, and a generation advances exactly when its
 * published row or equipped marker changes. An equip swap rewrites the moved serial afterwards.
 */
[[nodiscard]] bool
finalize_equipment_transition(const AccountState& account,
                              std::size_t characterIndex,
                              std::uint64_t requestedInstanceSoid,
                              EquipmentMutationKind kind,
                              std::uint8_t expectedNativeSlot,
                              const family4_loadout::ResolvedLoadout& beforeLoadout,
                              CharacterState& after,
                              std::size_t& movedItemCount) noexcept {
    movedItemCount = 0;
    if (characterIndex >= account.characterCount || kind == EquipmentMutationKind::none) {
        return false;
    }

    AccountState candidate = account;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout placedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, placedAfter)
        || placedAfter.itemCount != beforeLoadout.itemCount) {
        return false;
    }

    ResolvedPosition beforeRequested{};
    ResolvedPosition afterRequested{};
    if (!find_resolved_position(beforeLoadout, requestedInstanceSoid, beforeRequested)
        || !find_resolved_position(placedAfter, requestedInstanceSoid, afterRequested)
        || beforeRequested.equipmentSlot != expectedNativeSlot
        || afterRequested.equipmentSlot != expectedNativeSlot
        || (kind == EquipmentMutationKind::equip
            && (beforeRequested.equipped || !afterRequested.equipped))
        || (kind == EquipmentMutationKind::unequip
            && (!beforeRequested.equipped || afterRequested.equipped))) {
        return false;
    }

    const auto count_move = [&](const authored_inventory::Item& item) noexcept {
        ResolvedPosition beforePosition{};
        ResolvedPosition afterPosition{};
        if (!find_resolved_position(beforeLoadout, item.instanceSoid, beforePosition)
            || !find_resolved_position(placedAfter, item.instanceSoid, afterPosition)
            || beforePosition.equipmentSlot != afterPosition.equipmentSlot) {
            return false;
        }
        movedItemCount += static_cast<std::size_t>(!same_position(beforePosition, afterPosition));
        return true;
    };
    for (const auto& item : after.equipment.slots) {
        if (item.has_value() && !count_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        if (!count_move(after.inventory.values[index])) {
            return false;
        }
    }

    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (movedItemCount == 0 || after.nextInventorySerial > kMaximumInventorySerial
        || movedItemCount > kMaximumInventorySerial - after.nextInventorySerial) {
        return false;
    }

    const auto stamp_move = [&](authored_inventory::Item& item) noexcept {
        ResolvedPosition beforePosition{};
        ResolvedPosition afterPosition{};
        if (!find_resolved_position(beforeLoadout, item.instanceSoid, beforePosition)
            || !find_resolved_position(placedAfter, item.instanceSoid, afterPosition)
            || beforePosition.equipmentSlot != afterPosition.equipmentSlot) {
            return false;
        }
        if (!same_position(beforePosition, afterPosition)) {
            item.mutationSerial = static_cast<std::int32_t>(after.nextInventorySerial++);
        }
        return true;
    };
    for (auto& item : after.equipment.slots) {
        if (item.has_value() && !stamp_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        if (!stamp_move(after.inventory.values[index])) {
            return false;
        }
    }

    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, checkedAfter)
        || checkedAfter.itemCount != placedAfter.itemCount) {
        return false;
    }
    for (const auto& item : after.equipment.slots) {
        if (!item.has_value()) {
            continue;
        }
        ResolvedPosition placed{};
        ResolvedPosition checked{};
        if (!find_resolved_position(placedAfter, item->instanceSoid, placed)
            || !find_resolved_position(checkedAfter, item->instanceSoid, checked)
            || !same_position(placed, checked) || checked.mutationSerial != item->mutationSerial) {
            return false;
        }
    }
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const authored_inventory::Item& item = after.inventory.values[index];
        ResolvedPosition placed{};
        ResolvedPosition checked{};
        if (!find_resolved_position(placedAfter, item.instanceSoid, placed)
            || !find_resolved_position(checkedAfter, item.instanceSoid, checked)
            || !same_position(placed, checked) || checked.mutationSerial != item.mutationSerial) {
            return false;
        }
    }
    return true;
}

/** @return True when two authored item values are identical, including socket policy and tail. */
[[nodiscard]] bool same_item(const authored_inventory::Item& left,
                             const authored_inventory::Item& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.level == right.level && left.quantity == right.quantity
           && left.flags == right.flags && left.sockets.policy == right.sockets.policy
           && left.sockets.plugCount == right.sockets.plugCount
           && left.sockets.plugs == right.sockets.plugs
           && left.movementAbilityEntry == right.movementAbilityEntry
           && left.grenadeAbilityEntry == right.grenadeAbilityEntry
           && left.superAbilityEntry == right.superAbilityEntry
           && left.meleeAbilityEntry == right.meleeAbilityEntry
           && left.classAbilityEntry == right.classAbilityEntry;
}

/** Records one checked native item-state transition. */
void report_item_state(std::string_view stage,
                       std::string_view result,
                       std::string_view reason,
                       std::uint64_t characterSoid,
                       std::uint64_t instanceSoid,
                       std::uint16_t definitionIndex,
                       std::uint32_t beforeFlags,
                       std::uint32_t afterFlags,
                       bool equipped,
                       std::size_t itemIndex) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=item_state stage=%.*s result=%.*s reason=%.*s character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned>(definitionIndex),
        beforeFlags,
        afterFlags,
        equipped ? 1U : 0U,
        itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Exact stationary-item comparison, including its inventory mutation generation. */
[[nodiscard]] bool same_stationary_item(const authored_inventory::Item& left,
                                        const authored_inventory::Item& right) noexcept {
    return same_item(left, right) && left.mutationSerial == right.mutationSerial;
}

/** @return True when every item-bearing field of two character views is identical. */
[[nodiscard]] bool same_loadout(const CharacterState& left, const CharacterState& right) noexcept {
    if (left.soid != right.soid || left.selected != right.selected || left.race != right.race
        || left.gender != right.gender || left.characterClass != right.characterClass
        || left.level != right.level || left.accepted != right.accepted
        || left.previewAvailable != right.previewAvailable
        || left.appearanceValue != right.appearanceValue
        || left.lastOrbitedDestination != right.lastOrbitedDestination
        || left.contentBypass != right.contentBypass
        || left.presentationHeader != right.presentationHeader
        || left.creationHeader != right.creationHeader || left.creationTail != right.creationTail
        || left.creatorTrailer != right.creatorTrailer
        || left.nextInventorySerial != right.nextInventorySerial
        || left.inventory.count != right.inventory.count) {
        return false;
    }
    for (std::size_t index = 0; index < left.equipment.slots.size(); ++index) {
        const auto& leftItem = left.equipment.slots[index];
        const auto& rightItem = right.equipment.slots[index];
        if (leftItem.has_value() != rightItem.has_value()
            || (leftItem.has_value() && !same_stationary_item(*leftItem, *rightItem))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.inventory.count; ++index) {
        if (!same_stationary_item(left.inventory.values[index], right.inventory.values[index])) {
            return false;
        }
    }
    return true;
}

/** Exact character comparison, including the canonical unused inventory tail. */
[[nodiscard]] bool same_character(const CharacterState& left,
                                  const CharacterState& right) noexcept {
    if (!same_loadout(left, right)) {
        return false;
    }
    for (std::size_t index = left.inventory.count; index < left.inventory.values.size(); ++index) {
        if (!same_stationary_item(left.inventory.values[index], right.inventory.values[index])) {
            return false;
        }
    }
    return true;
}

/** Finds one item SOID exactly once in the character's equipment and dense inventory. */
[[nodiscard]] bool find_character_item_location(const CharacterState& character,
                                                std::uint64_t instanceSoid,
                                                CharacterItemLocation& location) noexcept {
    location = {};
    bool found = false;
    for (std::size_t index = 0; index < character.equipment.slots.size(); ++index) {
        const auto& item = character.equipment.slots[index];
        if (!item.has_value() || item->instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {index, true};
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {index, false};
    }
    return found;
}

/** Borrows an item at a previously validated authored location. */
[[nodiscard]] const authored_inventory::Item*
character_item_at(const CharacterState& character, const CharacterItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    if (location.index >= character.inventory.count) {
        return nullptr;
    }
    return &character.inventory.values[location.index];
}

/** Borrows a mutable item at a previously validated authored location. */
[[nodiscard]] authored_inventory::Item*
character_item_at(CharacterState& character, const CharacterItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    if (location.index >= character.inventory.count) {
        return nullptr;
    }
    return &character.inventory.values[location.index];
}

} // namespace runtime::detail
} // namespace sunrise::state
