#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "runtime.h"
#include "state.h"
#include "storage/internal.h"
#include "../build_data/runtime.h"
#include "../../core/filesystem/path.h"

namespace sunrise::state {

namespace {
constexpr std::uint32_t kInventoryStateMagic = 0x564E4953U;
constexpr std::uint32_t kInventoryStateVersion = 1;
struct InventoryStateHeader {
    std::uint32_t magic{}, version{}, recordSize{}, characterCount{};
};
struct InventoryCharacterRecord {
    std::uint64_t characterSoid{};
    account::inventory::Equipment equipment{};
    account::inventory::CharacterInventory inventory{};
};
}

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

/** @return A copy of the active account state, read under the lock. */
AccountState account_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState snapshot = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

bool reacquire_collection_item(std::uint16_t definitionIndex,
                               std::uint64_t& instanceSoid) noexcept {
    instanceSoid = 0;
    build_data::items::Definition definition{};
    build_data::items::details::Definition detail{};
    if (!build_data::find_item_definition_index(definitionIndex, definition)
        || !build_data::find_configured_item_detail(definitionIndex, detail)
        || definition.definitionHash != detail.definitionHash
        || definition.bucketId != detail.bucketId || !detail.equipmentSlot.has_value()) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    CharacterState* selected = nullptr;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (candidate.characters[index].selected) selected = &candidate.characters[index];
    }
    if (selected == nullptr || selected->inventory.itemCount >= selected->inventory.items.size()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    constexpr std::size_t kUnequippedPerSlot = 9;
    std::size_t matching = 0;
    std::uint64_t largestSoid = 0x4000000000000000ULL;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        const CharacterState& character = candidate.characters[characterIndex];
        for (const auto& equipped : character.equipment.slots) {
            if (equipped.has_value()) largestSoid = (std::max)(largestSoid, equipped->instanceSoid);
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.itemCount; ++itemIndex) {
            const auto& item = character.inventory.items[itemIndex];
            largestSoid = (std::max)(largestSoid, item.instanceSoid);
            build_data::items::Definition ownedDefinition{};
            build_data::items::details::Definition ownedDetail{};
            if (&character == selected
                && build_data::find_item_definition_hash(item.definitionHash, ownedDefinition)
                && build_data::find_configured_item_detail(ownedDefinition.definitionIndex,
                                                           ownedDetail)
                && ownedDetail.equipmentSlot == detail.equipmentSlot) {
                ++matching;
            }
        }
    }
    if (matching >= kUnequippedPerSlot || largestSoid == UINT64_MAX) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    account::inventory::Item item{};
    item.instanceSoid = largestSoid + 1;
    item.definitionHash = definition.definitionHash;
    item.level = selected->level;
    item.quantity = 1;
    item.sockets.policy = account::inventory::SocketPolicy::nativeDefaults;
    selected->inventory.items[selected->inventory.itemCount++] = item;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    runtime::storage::g_state.account = candidate;
    instanceSoid = item.instanceSoid;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    (void)persist_inventory_state();
    return true;
}

bool persist_inventory_state() noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&persist_inventory_state), &module)) return false;
    core::path::Buffer path{};
    if (!core::path::artifact_directory(module, path)
        || !core::path::append(path, L"\\inventory_state.bin")) return false;
    const AccountState snapshot = account_snapshot();
    if (!account::valid(snapshot)) return false;
    const HANDLE file = CreateFileW(path.chars.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const InventoryStateHeader header{kInventoryStateMagic, kInventoryStateVersion,
                                      sizeof(InventoryCharacterRecord),
                                      static_cast<std::uint32_t>(snapshot.characterCount)};
    DWORD written = 0;
    bool complete = WriteFile(file, &header, sizeof header, &written, nullptr) != FALSE
                    && written == sizeof header;
    for (std::size_t index = 0; complete && index < snapshot.characterCount; ++index) {
        const CharacterState& character = snapshot.characters[index];
        const InventoryCharacterRecord record{character.soid, character.equipment,
                                              character.inventory};
        complete = WriteFile(file, &record, sizeof record, &written, nullptr) != FALSE
                   && written == sizeof record;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return complete;
}

bool apply_item_plug(std::uint64_t instanceSoid,
                     std::uint8_t socketLane,
                     std::uint16_t plugDefinitionIndex) noexcept {
    build_data::items::Definition plugDefinition{};
    if (!build_data::find_item_definition_index(plugDefinitionIndex, plugDefinition)) return false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    account::inventory::Item* target = nullptr;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount; ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        for (auto& equipped : character.equipment.slots) {
            if (equipped.has_value() && equipped->instanceSoid == instanceSoid) target = &*equipped;
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.itemCount; ++itemIndex) {
            if (character.inventory.items[itemIndex].instanceSoid == instanceSoid)
                target = &character.inventory.items[itemIndex];
        }
    }
    build_data::items::Definition baseDefinition{};
    build_data::items::details::Definition detail{};
    if (target == nullptr
        || !build_data::find_item_definition_hash(target->definitionHash, baseDefinition)
        || !build_data::find_configured_item_detail(baseDefinition.definitionIndex, detail)
        || socketLane >= detail.ordinarySocketCount || socketLane >= target->sockets.plugs.size()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    account::inventory::Sockets sockets = target->sockets;
    if (sockets.policy != account::inventory::SocketPolicy::authored) {
        sockets = {};
        sockets.policy = account::inventory::SocketPolicy::authored;
        sockets.plugCount = detail.ordinarySocketCount;
        for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
            const std::uint16_t initial = detail.initialPlugIndices[lane];
            if (initial == build_data::items::details::kUnavailableItemIndex) continue;
            build_data::items::Definition initialDefinition{};
            if (!build_data::find_item_definition_index(initial, initialDefinition)) {
                ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
                return false;
            }
            sockets.plugs[lane] = initialDefinition.definitionHash;
        }
    } else if (sockets.plugCount != detail.ordinarySocketCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    sockets.plugs[socketLane] = plugDefinition.definitionHash;
    target->sockets = sockets;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    (void)persist_inventory_state();
    return true;
}

} // namespace sunrise::state
