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
namespace socket_lists = build_data::socket_entry_lists;

/** Writes one exhaustive equipment-transaction checkpoint to the persistent diagnostic log. */
void report_equipment(std::string_view stage,
                      std::string_view result,
                      EquipmentMutationKind kind,
                      std::uint64_t characterSoid,
                      std::uint64_t previousSoid,
                      std::uint64_t requestedSoid,
                      std::size_t equipmentIndex,
                      std::size_t inventoryIndex,
                      std::uint8_t nativeSlot,
                      std::size_t movedItemCount,
                      std::uint32_t previousHash,
                      std::uint32_t requestedHash) noexcept {
    const std::string_view operation = kind == EquipmentMutationKind::unequip ? "unequip" : "equip";
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=equip operation=%.*s stage=%.*s result=%.*s character=0x%llX previous=0x%llX "
        "requested=0x%llX equipment_index=%zu inventory_index=%zu native_slot=%u "
        "moved_items=%zu previous_hash=0x%08X requested_hash=0x%08X",
        static_cast<int>(operation.size()),
        operation.data(),
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(previousSoid),
        static_cast<unsigned long long>(requestedSoid),
        equipmentIndex,
        inventoryIndex,
        static_cast<unsigned>(nativeSlot),
        movedItemCount,
        previousHash,
        requestedHash);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Writes one exhaustive item-acquisition transaction checkpoint. */
void report_acquisition(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint32_t definitionHash,
                        std::uint64_t characterSoid,
                        std::uint64_t instanceSoid,
                        std::size_t inventoryIndex,
                        std::uint16_t inventoryRow,
                        std::uint8_t equipmentSlot,
                        std::uint32_t nextInventorySerial) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X character=0x%llX "
        "instance=0x%llX inventory_index=%zu inventory_row=%u equipment_slot=%u next_serial=%u",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        definitionHash,
        static_cast<unsigned long long>(characterSoid),
        static_cast<unsigned long long>(instanceSoid),
        inventoryIndex,
        static_cast<unsigned>(inventoryRow),
        static_cast<unsigned>(equipmentSlot),
        nextInventorySerial);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/**
 * Prepares a subclass ability-entry transition without publishing account State.
 * The requested entry must share a socket-entry group with exactly one of the character's 5
 * authored picks; that pick is updated, mirroring how `resolve_socket_states` reads a selection.
 */
[[nodiscard]] bool stage_subclass_selection(const AccountState& snapshot,
                                            std::size_t characterIndex,
                                            std::uint64_t subclassInstanceSoid,
                                            std::uint8_t requestedEntry,
                                            PendingSubclassSelection& mutation) noexcept {
    mutation = {};
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || subclassInstanceSoid == 0 || requestedEntry >= socket_lists::kEntryCapacity) {
        return false;
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return false;
    }
    // Index of the subclass slot in the authored equipment array.
    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::subclass);
    const auto& subclass = before.equipment.slots[kSubclassSlot];
    build_data::items::Definition subclassDefinition{};
    item_details::Definition detail{};
    socket_lists::EntryTable entries{};
    if (!subclass.has_value() || subclass->instanceSoid != subclassInstanceSoid
        || !build_data::find_item_definition_hash(subclass->definitionHash, subclassDefinition)
        || !build_data::find_configured_item_detail(subclassDefinition.definitionIndex, detail)
        || !build_data::find_socket_entry_table(detail.socketEntryListIndex, entries)
        || requestedEntry >= entries.entries.size()) {
        return false;
    }

    const socket_lists::Entry& requested = entries.entries[requestedEntry];
    if (requested.plugSource == socket_lists::kNoPlugSource
        || requested.group == socket_lists::kNoEntryGroup) {
        return false;
    }

    // A clicked entry's table position does not say which ability slot it fills; only its resolved
    // destination bucket does. A bundled pick can mix members across slots, so every member of the
    // clicked entry's bundle is checked, not just the one clicked.
    CharacterState after = before;
    // The picks belong to the equipped subclass item itself, not the character, so each owned
    // subclass remembers its own selection independently instead of sharing one set across all
    // of them.
    auto& afterSubclass = after.equipment.slots[kSubclassSlot];
    struct Route {
        std::uint8_t bucket;
        std::uint8_t* field;
        std::uint8_t defaultEntry;
    };
    const std::array<Route, 5> routes{{
        {kMovementAbilityBucket,
         &afterSubclass->movementAbilityEntry,
         kDefaultMovementAbilityEntry},
        {kGrenadeAbilityBucket, &afterSubclass->grenadeAbilityEntry, kDefaultGrenadeAbilityEntry},
        {kSuperAbilityBucket, &afterSubclass->superAbilityEntry, kDefaultSuperAbilityEntry},
        {kMeleeAbilityBucket, &afterSubclass->meleeAbilityEntry, kDefaultMeleeAbilityEntry},
        {class_ability_bucket(after.characterClass),
         &afterSubclass->classAbilityEntry,
         kDefaultClassAbilityEntry},
    }};
    const auto bucket_of = [&](std::uint8_t entryIndex) noexcept {
        std::uint8_t bucket = build_data::socket_entry_buckets::kNoDestinationBucket;
        (void)build_data::find_socket_entry_bucket(detail.socketEntryListIndex, entryIndex, bucket);
        return bucket;
    };
    const auto route_entry = [&](std::uint8_t entryIndex) noexcept {
        const std::uint8_t bucket = bucket_of(entryIndex);
        for (const Route& route : routes) {
            if (route.bucket == bucket) {
                *route.field = entryIndex;
                return;
            }
        }
    };
    // A click can land on any member of a bundle, not only the routable one: the other quadrants
    // are passive nodes with no destination bucket. The bundle's start is found by scanning
    // backward, then every member is routed from there, and only while the group is wide.
    std::size_t groupPopulation = 0;
    for (std::size_t index = 0; index < entries.entries.size(); ++index) {
        if (entries.entries[index].group == requested.group) {
            ++groupPopulation;
        }
    }
    if (groupPopulation <= kMaxAttunementBundleSize) {
        route_entry(requestedEntry);
    } else {
        // A wide group is several same-sized bundles competing for one pick, so only one bundle's
        // fields stay set. A bundle that does not touch every field the group reaches must not
        // leave an earlier bundle's value behind, so every bucket is reset before the pick writes.
        for (std::size_t index = 0; index < entries.entries.size(); ++index) {
            if (entries.entries[index].group != requested.group) {
                continue;
            }
            const std::uint8_t bucket = bucket_of(static_cast<std::uint8_t>(index));
            for (const Route& route : routes) {
                if (route.bucket == bucket) {
                    *route.field = route.defaultEntry;
                }
            }
        }
        std::uint8_t blockStart = requestedEntry;
        while (blockStart > 0 && requestedEntry - blockStart < kMaxAttunementBundleSize - 1
               && entries.entries[blockStart - 1].group == requested.group) {
            --blockStart;
        }
        for (std::size_t offset = 0;
             offset < kMaxAttunementBundleSize && blockStart + offset < entries.entries.size()
             && entries.entries[blockStart + offset].group == requested.group;
             ++offset) {
            route_entry(static_cast<std::uint8_t>(blockStart + offset));
        }
    }
    if (same_character(before, after)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.subclassInstanceSoid = subclassInstanceSoid;
    mutation.subclassDefinitionHash = subclassDefinition.definitionHash;
    mutation.characterIndex = characterIndex;
    mutation.subclassDefinitionIndex = subclassDefinition.definitionIndex;
    mutation.socketEntryListIndex = detail.socketEntryListIndex;
    mutation.requestedEntry = requestedEntry;
    mutation.prepared = true;
    return true;
}

} // namespace runtime::detail

using namespace runtime::detail;

/** Stores the active account key without publishing an incomplete account. */
bool set_primary_soid(std::uint64_t primarySoid) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    candidate.primarySoid = primarySoid;
    // Characters belong to the account key the Client uses. The reference account and its
    // characters differ only in the low byte, so the authored rows are rebased onto that key.
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        candidate.characters[index].soid = primarySoid + 1U + index;
    }
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the settings and identity rules hold together.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Closes the account's one-time profile-setup gate. */
bool complete_profile_setup() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState& accountState = runtime::storage::g_state.account;
    if (accountState.primarySoid == 0 || !account::valid(accountState)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool changed = !accountState.profileSetupCompleted;
    accountState.profileSetupCompleted = true;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);

    if (changed) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         "ev=profile_setup stage=complete result=ok");
    }
    return true;
}

/** Moves the selection to one authored character. */
bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept {
    changed = false;
    if (characterSoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    std::size_t picked = candidate.characterCount;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (candidate.characters[index].soid == characterSoid) {
            picked = index;
        }
    }
    if (picked == candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool alreadySelected = candidate.characters[picked].selected;
    for (CharacterState& character : candidate.characters) {
        character.selected = false;
    }
    candidate.characters[picked].selected = true;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the whole account still meets its identity rules.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    changed = !alreadySelected;
    return true;
}

/** Prepares one checked equip transition without changing account State. */
bool prepare_equipment_swap(std::uint64_t requestedInstanceSoid,
                            PendingEquipmentSwap& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (requestedInstanceSoid == 0 || !account::valid(account)) {
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)) {
        return false;
    }

    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        if (before.inventory.values[index].instanceSoid != requestedInstanceSoid) {
            continue;
        }
        if (inventoryIndex != before.inventory.count) {
            return false;
        }
        inventoryIndex = index;
    }
    if (inventoryIndex == before.inventory.count) {
        return false;
    }

    const authored_inventory::Item& requested = before.inventory.values[inventoryIndex];
    std::uint8_t requestedNativeSlot = 0;
    std::size_t equipmentSlotIndex = authored_inventory::kEquipmentSlotCount;
    ResolvedPosition requestedPosition{};
    if (!native_equipment_slot(requested, requestedNativeSlot)
        || !semantic_equipment_slot(requestedNativeSlot, equipmentSlotIndex)
        || !find_resolved_position(beforeLoadout, requestedInstanceSoid, requestedPosition)
        || requestedPosition.equipped || requestedPosition.equipmentSlot != requestedNativeSlot) {
        return false;
    }

    CharacterState after = before;
    auto& equipped = after.equipment.slots[equipmentSlotIndex];
    std::uint64_t previousInstanceSoid = 0;
    std::uint32_t previousDefinitionHash = 0;
    if (equipped.has_value()) {
        std::uint8_t previousNativeSlot = 0;
        ResolvedPosition previousPosition{};
        if (!native_equipment_slot(*equipped, previousNativeSlot)
            || previousNativeSlot != requestedNativeSlot
            || !find_resolved_position(beforeLoadout, equipped->instanceSoid, previousPosition)
            || !previousPosition.equipped
            || previousPosition.equipmentSlot != requestedNativeSlot) {
            return false;
        }
        previousInstanceSoid = equipped->instanceSoid;
        previousDefinitionHash = equipped->definitionHash;
        std::swap(*equipped, after.inventory.values[inventoryIndex]);
    } else {
        equipped = after.inventory.values[inventoryIndex];
        for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
            after.inventory.values[index] = after.inventory.values[index + 1U];
        }
        --after.inventory.count;
        after.inventory.values[after.inventory.count] = {};
    }

    std::size_t movedItemCount = 0;
    if (!finalize_equipment_transition(account,
                                       characterIndex,
                                       requestedInstanceSoid,
                                       EquipmentMutationKind::equip,
                                       requestedNativeSlot,
                                       beforeLoadout,
                                       after,
                                       movedItemCount)) {
        return false;
    }

    if (previousInstanceSoid != 0) {
        // The serial on an unequipped row is also the Client's ordering token for that bucket. A
        // fresh greatest serial would move the displaced item to the first cell, so transfer the
        // selected row's prior token instead and it keeps the cell the player clicked.
        authored_inventory::Item& displaced = after.inventory.values[inventoryIndex];
        if (displaced.instanceSoid != previousInstanceSoid) {
            return false;
        }
        displaced.mutationSerial = requestedPosition.mutationSerial;

        AccountState checkedAccount = account;
        checkedAccount.characters[characterIndex] = after;
        family4_loadout::ResolvedLoadout checkedLoadout{};
        ResolvedPosition displacedPosition{};
        if (!account::valid(checkedAccount)
            || !family4_loadout::resolve(checkedAccount, characterIndex, checkedLoadout)
            || !find_resolved_position(checkedLoadout, previousInstanceSoid, displacedPosition)
            || displacedPosition.equipped || displacedPosition.equipmentSlot != requestedNativeSlot
            || displacedPosition.inventoryRow != requestedPosition.inventoryRow
            || displacedPosition.mutationSerial != requestedPosition.mutationSerial) {
            return false;
        }
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.previousInstanceSoid = previousInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.equipmentSlotIndex = equipmentSlotIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedItemCount = movedItemCount;
    mutation.nativeEquipmentSlot = requestedNativeSlot;
    mutation.kind = EquipmentMutationKind::equip;
    mutation.prepared = true;
    report_equipment("prepare",
                     "ok",
                     mutation.kind,
                     mutation.characterSoid,
                     mutation.previousInstanceSoid,
                     mutation.requestedInstanceSoid,
                     mutation.equipmentSlotIndex,
                     mutation.inventoryIndex,
                     mutation.nativeEquipmentSlot,
                     mutation.movedItemCount,
                     previousDefinitionHash,
                     requested.definitionHash);
    return true;
}

/** Prepares one checked equipped-to-inventory transition without changing account State. */
bool prepare_equipment_unequip(std::uint64_t requestedInstanceSoid,
                               PendingEquipmentSwap& mutation) noexcept {
    mutation = {};
    const AccountState account = account_snapshot();
    if (requestedInstanceSoid == 0 || !account::valid(account)) {
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        return false;
    }

    const CharacterState& before = account.characters[characterIndex];
    if (before.inventory.count >= before.inventory.values.size()) {
        return false;
    }
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!family4_loadout::resolve(account, characterIndex, beforeLoadout)) {
        return false;
    }

    std::size_t equipmentSlotIndex = before.equipment.slots.size();
    for (std::size_t index = 0; index < before.equipment.slots.size(); ++index) {
        const auto& item = before.equipment.slots[index];
        if (!item.has_value() || item->instanceSoid != requestedInstanceSoid) {
            continue;
        }
        if (equipmentSlotIndex != before.equipment.slots.size()) {
            return false;
        }
        equipmentSlotIndex = index;
    }
    if (equipmentSlotIndex == before.equipment.slots.size()) {
        return false;
    }

    const authored_inventory::Item& requested = *before.equipment.slots[equipmentSlotIndex];
    std::uint8_t requestedNativeSlot = 0;
    std::uint8_t requestedBucketId = 0;
    std::size_t expectedSemanticIndex = authored_inventory::kEquipmentSlotCount;
    ResolvedPosition requestedPosition{};
    if (!native_equipment_slot(requested, requestedNativeSlot)
        || !inventory_bucket_id(requested, requestedBucketId)
        || !semantic_equipment_slot(requestedNativeSlot, expectedSemanticIndex)
        || expectedSemanticIndex != equipmentSlotIndex
        || !find_resolved_position(beforeLoadout, requestedInstanceSoid, requestedPosition)
        || !requestedPosition.equipped || requestedPosition.equipmentSlot != requestedNativeSlot) {
        return false;
    }

    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        std::uint8_t inventoryBucketId = 0;
        if (!inventory_bucket_id(before.inventory.values[index], inventoryBucketId)) {
            return false;
        }
        if (inventoryBucketId == requestedBucketId) {
            inventoryIndex = index;
            break;
        }
    }

    CharacterState after = before;
    const authored_inventory::Item unequipped = *after.equipment.slots[equipmentSlotIndex];
    for (std::size_t index = after.inventory.count; index > inventoryIndex; --index) {
        after.inventory.values[index] = after.inventory.values[index - 1U];
    }
    after.inventory.values[inventoryIndex] = unequipped;
    ++after.inventory.count;
    after.equipment.slots[equipmentSlotIndex].reset();

    std::size_t movedItemCount = 0;
    if (!finalize_equipment_transition(account,
                                       characterIndex,
                                       requestedInstanceSoid,
                                       EquipmentMutationKind::unequip,
                                       requestedNativeSlot,
                                       beforeLoadout,
                                       after,
                                       movedItemCount)) {
        return false;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.characterSoid = before.soid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.equipmentSlotIndex = equipmentSlotIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.movedItemCount = movedItemCount;
    mutation.nativeEquipmentSlot = requestedNativeSlot;
    mutation.kind = EquipmentMutationKind::unequip;
    mutation.prepared = true;
    report_equipment("prepare",
                     "ok",
                     mutation.kind,
                     mutation.characterSoid,
                     mutation.previousInstanceSoid,
                     mutation.requestedInstanceSoid,
                     mutation.equipmentSlotIndex,
                     mutation.inventoryIndex,
                     mutation.nativeEquipmentSlot,
                     mutation.movedItemCount,
                     0,
                     requested.definitionHash);
    return true;
}

/** Commits one prepared equipment after-image behind an exact character staleness guard. */
bool commit_equipment_swap(PendingEquipmentSwap& mutation) noexcept {
    const PendingEquipmentSwap prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.characterSoid == 0 || prepared.requestedInstanceSoid == 0
        || (prepared.kind != EquipmentMutationKind::equip
            && prepared.kind != EquipmentMutationKind::unequip)
        || prepared.characterIndex >= kCharacterCapacity
        || prepared.equipmentSlotIndex >= authored_inventory::kEquipmentSlotCount
        || prepared.inventoryIndex >= authored_inventory::kCharacterItemCapacity
        || prepared.nativeEquipmentSlot >= item_details::kEquipmentSlotCount
        || prepared.beforeCharacter.soid != prepared.characterSoid
        || prepared.afterCharacter.soid != prepared.characterSoid) {
        return false;
    }

    const std::uint32_t previousDefinitionHash =
        character_item_definition_hash(prepared.afterCharacter, prepared.previousInstanceSoid);
    const std::uint32_t requestedDefinitionHash =
        character_item_definition_hash(prepared.afterCharacter, prepared.requestedInstanceSoid);
    report_equipment("commit_begin",
                     "ok",
                     prepared.kind,
                     prepared.characterSoid,
                     prepared.previousInstanceSoid,
                     prepared.requestedInstanceSoid,
                     prepared.equipmentSlotIndex,
                     prepared.inventoryIndex,
                     prepared.nativeEquipmentSlot,
                     prepared.movedItemCount,
                     previousDefinitionHash,
                     requestedDefinitionHash);

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (prepared.characterIndex >= candidate.characterCount
        || !same_character(candidate.characters[prepared.characterIndex],
                           prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    candidate.characters[prepared.characterIndex] = prepared.afterCharacter;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(candidate)
        || !family4_loadout::resolve(candidate, prepared.characterIndex, checkedAfter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    ResolvedPosition requestedPosition{};
    const bool expectedEquipped = prepared.kind == EquipmentMutationKind::equip;
    if (!find_resolved_position(checkedAfter, prepared.requestedInstanceSoid, requestedPosition)
        || requestedPosition.equipmentSlot != prepared.nativeEquipmentSlot
        || requestedPosition.equipped != expectedEquipped) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);

    // The published ability buckets resolve against whichever subclass is equipped, so swapping
    // that item away makes the domain stale the same way an ability-entry pick does. It needs the
    // same invalidation or the character screen keeps showing the previous resolution.
    if (prepared.equipmentSlotIndex
        == static_cast<std::size_t>(authored_inventory::EquipmentSlot::subclass)) {
        build_data::invalidate_ability_buckets();
    }

    report_equipment("commit_end",
                     "ok",
                     prepared.kind,
                     prepared.characterSoid,
                     prepared.previousInstanceSoid,
                     prepared.requestedInstanceSoid,
                     prepared.equipmentSlotIndex,
                     prepared.inventoryIndex,
                     prepared.nativeEquipmentSlot,
                     prepared.movedItemCount,
                     previousDefinitionHash,
                     requestedDefinitionHash);
    return true;
}

/** @return A copy of the active account state, read under the lock. */
AccountState account_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState snapshot = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

/** Grants each character the other 2 subclasses of its equipped subclass's class. */
bool ensure_character_subclasses() noexcept {
    // Index of the subclass slot in the authored equipment array.
    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::subclass);
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return true;
    }
    std::uint64_t nextSoid = 0;
    bool haveNextSoid = false;
    bool changed = false;
    bool failed = false;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount && !failed;
         ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        const std::optional<authored_inventory::Item>& equipped =
            character.equipment.slots[kSubclassSlot];
        if (!equipped.has_value()) {
            continue;
        }
        build_data::items::Definition equippedDefinition{};
        std::array<std::uint16_t, build_data::kSubclassGroupSize> group{};
        if (!build_data::find_item_definition_hash(equipped->definitionHash, equippedDefinition)
            || !build_data::find_subclass_group(equippedDefinition.definitionIndex, group)) {
            continue;
        }
        for (const std::uint16_t memberIndex : group) {
            if (memberIndex == equippedDefinition.definitionIndex) {
                continue;
            }
            build_data::items::Definition memberDefinition{};
            if (!build_data::find_item_definition_index(memberIndex, memberDefinition)
                || memberDefinition.definitionIndex != memberIndex) {
                continue;
            }
            bool present = false;
            for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
                if (character.inventory.values[itemIndex].definitionHash
                    == memberDefinition.definitionHash) {
                    present = true;
                    break;
                }
            }
            if (present || character.inventory.count >= character.inventory.values.size()) {
                continue;
            }
            if (!haveNextSoid) {
                if (!next_item_instance_soid(candidate, nextSoid)) {
                    failed = true;
                    break;
                }
                haveNextSoid = true;
            }
            authored_inventory::Item granted{};
            granted.instanceSoid = nextSoid++;
            granted.definitionHash = memberDefinition.definitionHash;
            granted.level = 0;
            granted.quantity = 1;
            // Every resolved item's serial must stay behind the character's own counter (checked
            // by the character encoder, not by account::valid), so claim the next one here too.
            granted.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
            granted.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
            character.inventory.values[character.inventory.count++] = granted;
            changed = true;
        }
    }
    if (failed || !changed || !account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return !failed;
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

} // namespace sunrise::state
