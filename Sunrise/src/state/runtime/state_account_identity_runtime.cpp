/** Instance identity helpers: generated SOIDs, ownership tests, and loadout row lookups. */

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

/** First SOID reserved for item instances created by this local runtime. */
constexpr std::uint64_t kFirstGeneratedItemSoid = 0x4000000000000001ULL;

/** Returns one character-owned instance's definition hash for bounded transaction diagnostics. */
[[nodiscard]] std::uint32_t character_item_definition_hash(const CharacterState& character,
                                                           std::uint64_t instanceSoid) noexcept {
    for (const auto& item : character.equipment.slots) {
        if (item.has_value() && item->instanceSoid == instanceSoid) {
            return item->definitionHash;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid == instanceSoid) {
            return character.inventory.values[index].definitionHash;
        }
    }
    return 0;
}

/** @return True when any account, character, profile-stack, or character-item key owns one SOID. */
[[nodiscard]] bool account_owns_soid(const AccountState& account, std::uint64_t soid) noexcept {
    if (soid == 0 || account.primarySoid == soid) {
        return true;
    }
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        if (account.profileItems[index].instanceSoid == soid) {
            return true;
        }
    }
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        const CharacterState& character = account.characters[characterIndex];
        if (character.soid == soid) {
            return true;
        }
        for (const auto& item : character.equipment.slots) {
            if (item.has_value() && item->instanceSoid == soid) {
                return true;
            }
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            if (character.inventory.values[index].instanceSoid == soid) {
                return true;
            }
        }
    }
    return false;
}

/** Finds a fresh deterministic item-instance SOID without sharing any other identity key. */
[[nodiscard]] bool next_item_instance_soid(const AccountState& account,
                                           std::uint64_t& output) noexcept {
    std::uint64_t candidate = kFirstGeneratedItemSoid;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        const CharacterState& character = account.characters[characterIndex];
        for (const auto& item : character.equipment.slots) {
            if (!item.has_value() || item->instanceSoid < candidate) {
                continue;
            }
            if (item->instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            candidate = item->instanceSoid + 1U;
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            const std::uint64_t instanceSoid = character.inventory.values[index].instanceSoid;
            if (instanceSoid < candidate) {
                continue;
            }
            if (instanceSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            candidate = instanceSoid + 1U;
        }
    }
    while (account_owns_soid(account, candidate)) {
        if (candidate == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        ++candidate;
    }
    output = candidate;
    return output != 0;
}

/** Finds a collision-free SOID for one newly appended profile stack. */
[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept {
    std::uint64_t candidate = authored_inventory::kFirstProfileItemInstanceSoid;
    while (account_owns_soid(account, candidate)) {
        if (candidate == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        ++candidate;
    }
    output = candidate;
    return output != 0;
}

/** @return True when an account or character identity already owns the candidate object key. */
[[nodiscard]] bool identity_uses_soid(const AccountState& account, std::uint64_t soid) noexcept {
    if (soid == 0 || account.primarySoid == soid) {
        return true;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == soid) {
            return true;
        }
    }
    return false;
}

/** Uses the character's strongest existing item as the neutral local Collections pull level. */
[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept {
    std::int32_t level = 0;
    for (const auto& item : character.equipment.slots) {
        if (item.has_value()) {
            level = (std::max)(level, item->level);
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        level = (std::max)(level, character.inventory.values[index].level);
    }
    return level;
}

/** Finds the one resolved unequipped row created by an acquisition candidate. */
[[nodiscard]] bool find_acquired_row(const family4_loadout::ResolvedLoadout& loadout,
                                     std::uint64_t instanceSoid,
                                     std::uint16_t& inventoryRow,
                                     std::uint8_t& equipmentSlot) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found || item.equipped) {
            return false;
        }
        found = true;
        inventoryRow = item.inventoryRow;
        equipmentSlot = item.equipmentSlot;
    }
    return found;
}

/** Finds the unique resolved unequipped position for one instance. */
[[nodiscard]] bool find_unequipped_row(const family4_loadout::ResolvedLoadout& loadout,
                                       std::uint64_t instanceSoid,
                                       std::uint16_t& inventoryRow,
                                       std::uint8_t& equipmentSlot) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found || item.equipped) {
            return false;
        }
        found = true;
        inventoryRow = item.inventoryRow;
        equipmentSlot = item.equipmentSlot;
    }
    return found;
}

/** @return True when the resolved loadout still carries an instance with this key. */
[[nodiscard]] bool loadout_contains(const family4_loadout::ResolvedLoadout& loadout,
                                    std::uint64_t instanceSoid) noexcept {
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        if (loadout.items[index].instance.instanceSoid == instanceSoid) {
            return true;
        }
    }
    return false;
}

} // namespace runtime::detail
} // namespace sunrise::state

#include "character_creation.inl"
#include "character_deletion.inl"
