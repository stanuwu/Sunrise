/** Character creation and deletion. */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "../../core/logging/log.h"
#include "../account/account_state.h"
#include "../account/inventory/inventory_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

namespace authored_inventory = account::inventory;

/** Sunrise's own starter rows one class may list. The bundled table lists one. */
constexpr std::size_t kAuthoredStarterCapacity = 16;
/** A powered item needs a nonzero level. Level 1 already scores the Power floor. */
constexpr std::int32_t kMinimumPoweredLevel = 1;

/** @return True for a weapon or armour slot; these lead the slot order and alone carry Power. */
[[nodiscard]] constexpr bool powered_slot(std::size_t slot) noexcept {
    return slot <= static_cast<std::size_t>(authored_inventory::EquipmentSlot::classItem);
}

/**
 * Picks the level a new character's weapons and armour get.
 * It is the strongest level any character already holds, so a new character keeps pace.
 * @param account Account before the new character joins.
 * @return That level, and never less than the powered minimum.
 */
[[nodiscard]] std::int32_t starter_level(const AccountState& account) noexcept {
    std::int32_t level = kMinimumPoweredLevel;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        level = (std::max)(level, runtime::detail::acquisition_level(account.characters[index]));
    }
    return level;
}

/**
 * Picks the lowest character id after the account's own that no character holds.
 * A delete moves survivors down a slot but keeps their ids, so the id cannot come from the slot.
 * @param account Account the new character joins.
 * @return The free id, or zero when every id is taken.
 */
[[nodiscard]] std::uint64_t free_character_soid(const AccountState& account) noexcept {
    const auto begin = account.characters.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(account.characterCount);
    for (std::uint64_t offset = 1; offset <= account.characters.size(); ++offset) {
        const std::uint64_t candidate = account.primarySoid + offset;
        const bool taken = std::any_of(begin, end, [candidate](const CharacterState& character) {
            return character.soid == candidate;
        });
        if (!taken) {
            return candidate;
        }
    }
    return 0;
}

/**
 * Resolves the semantic equipment slot one item takes.
 * @param definitionHash Item definition hash.
 * @param slot Receives the semantic slot.
 * @return False when the item is not installed or takes no supported slot.
 */
[[nodiscard]] bool starter_slot(std::uint32_t definitionHash, std::size_t& slot) noexcept {
    build_data::items::Definition definition{};
    build_data::items::details::Definition detail{};
    std::uint8_t nativeSlot = 0;
    return build_data::find_item_definition_hash(definitionHash, definition)
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && authored_inventory::resolve_native_equipment_slot(
               definitionHash, detail.equipmentSlot, nativeSlot)
           && runtime::detail::semantic_equipment_slot(nativeSlot, slot);
}

/**
 * Gives one starter item to a new character: equipped when its slot is free, else unequipped.
 * @param account Account that already counts the character, so a new item id cannot collide.
 * @param character The new character inside account.
 * @param definitionHash Item to give.
 * @param level Level a weapon or armour piece gets. Every other item gets none.
 * @return False when the item takes no slot, no item id is free, or the inventory is full.
 */
[[nodiscard]] bool give_starter_item(AccountState& account,
                                     CharacterState& character,
                                     std::uint32_t definitionHash,
                                     std::int32_t level) noexcept {
    std::size_t slot = 0;
    authored_inventory::Item item{};
    if (!starter_slot(definitionHash, slot)
        || !runtime::detail::next_item_instance_soid(account, item.instanceSoid)) {
        return false;
    }
    item.definitionHash = definitionHash;
    item.level = powered_slot(slot) ? level : 0;
    item.quantity = 1;
    item.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    std::optional<authored_inventory::Item>& equipped = character.equipment.slots[slot];
    if (!equipped.has_value()) {
        equipped = item;
        return true;
    }
    if (character.inventory.count >= character.inventory.values.size()) {
        return false;
    }
    character.inventory.values[character.inventory.count++] = item;
    return true;
}

/**
 * Gives a new character Sunrise's own starter rows, then the packages' new-character set.
 * The first item for a slot is equipped. A later item for the same slot stays unequipped.
 * @param account Account that already counts the character.
 * @param character The new character inside account.
 * @param level Level for weapons and armour.
 * @return False when either source does not read for the class, or one item cannot be given.
 */
[[nodiscard]] bool
seed_starter_set(AccountState& account, CharacterState& character, std::int32_t level) noexcept {
    const auto characterClass = static_cast<std::uint8_t>(character.characterClass);
    std::array<std::uint32_t, kAuthoredStarterCapacity> authored{};
    std::size_t authoredCount = 0;
    build_data::starter_sets::Definition packaged{};
    if (!investment::store::read_starter_items(characterClass, authored, authoredCount)
        || !build_data::find_starter_set(characterClass, packaged)) {
        return false;
    }
    for (std::size_t index = 0; index < authoredCount; ++index) {
        if (!give_starter_item(account, character, authored[index], level)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < packaged.itemCount; ++index) {
        build_data::items::Definition definition{};
        if (!build_data::find_item_definition_index(packaged.itemIndices[index], definition)
            || !give_starter_item(account, character, definition.definitionHash, level)) {
            return false;
        }
    }
    return true;
}

/** Logs one refused roster change with the stage that refused it. */
void report_refusal(const char* event, const char* stage) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=%s stage=%s result=fail",
                      event,
                      stage);
}

} // namespace

/**
 * Adds a character to a free slot with its starter set and unlocks, and saves the account.
 * The character joins unselected. Only a pick selects a character.
 * @param characterClass Character class: 0 Titan, 1 Hunter, 2 Warlock.
 * @param gender Character gender: 0 male, 1 female.
 * @param race Character race: 0 human, 1 awoken, 2 exo.
 * @param appearanceHeader Appearance header from the request, in record byte order.
 * @param characterSoid Receives the new character's id, zero on failure.
 * @return False when the roster is full, the starter set does not resolve, or the save refuses
 *         the account. The account check refuses an identity value out of range.
 */
bool create_character(std::uint8_t characterClass,
                      std::uint8_t gender,
                      std::uint8_t race,
                      const AppearanceHeader& appearanceHeader,
                      std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    const std::lock_guard lock(investment::store::g_mutex);
    AccountState candidate = investment::store::account();
    const std::uint64_t soid =
        candidate.primarySoid == 0 || candidate.characterCount >= candidate.characters.size()
            ? 0
            : free_character_soid(candidate);
    if (soid == 0) {
        report_refusal("create_character", "slot");
        return false;
    }
    const std::int32_t level = starter_level(candidate);
    const std::size_t slot = candidate.characterCount;
    CharacterState& character = candidate.characters[slot];
    character = {};
    character.soid = soid;
    character.race = static_cast<CharacterRace>(race);
    character.gender = static_cast<CharacterGender>(gender);
    character.characterClass = static_cast<CharacterClass>(characterClass);
    character.appearanceHeader = appearanceHeader;
    if (!investment::store::read_starter_character(character)) {
        report_refusal("create_character", "starter_character");
        return false;
    }
    // Counted before seeding, so each new item id sees the items given before it.
    ++candidate.characterCount;
    if (!seed_starter_set(candidate, character, level)) {
        report_refusal("create_character", "starter_set");
        return false;
    }
    // Both writes share one save transaction, so a refused one leaves neither.
    investment::store::Transaction transaction;
    if (!transaction.ready() || !investment::store::write_account(candidate)
        || !investment::store::seed_character_unlocks(slot) || !transaction.commit()) {
        report_refusal("create_character", "commit");
        return false;
    }
    // The new class's subclasses may have no ability row yet; the next refresh rebuilds them.
    build_data::invalidate_ability_buckets();
    characterSoid = soid;
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=create_character result=ok soid=0x%016llX chars=%zu",
                      static_cast<unsigned long long>(soid),
                      candidate.characterCount);
    return true;
}

/**
 * Removes one character, its items and its slot-keyed rows, and saves the account.
 * Survivors keep their ids and move down one slot. Deleting the selection leaves none selected.
 * @param characterSoid Id of the character to remove.
 * @param selectionMoved Set when the removed character was the selected one.
 * @return False when no character has that id or the save refuses the change.
 */
bool delete_character(std::uint64_t characterSoid, bool& selectionMoved) noexcept {
    selectionMoved = false;
    const std::lock_guard lock(investment::store::g_mutex);
    AccountState candidate = investment::store::account();
    const auto begin = candidate.characters.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(candidate.characterCount);
    const auto found = std::find_if(begin, end, [characterSoid](const CharacterState& character) {
        return character.soid == characterSoid;
    });
    if (characterSoid == 0 || found == end) {
        report_refusal("delete_character", "find");
        return false;
    }
    const auto slot = static_cast<std::size_t>(found - begin);
    const bool wasSelected = found->selected;
    // Each survivor takes its own selection flag down a slot; only a pick selects.
    std::move(found + 1, end, found);
    --candidate.characterCount;
    candidate.characters[candidate.characterCount] = {};
    // Both writes share one save transaction, so a refused one leaves neither.
    investment::store::Transaction transaction;
    if (!transaction.ready() || !investment::store::write_account(candidate)
        || !investment::store::remove_character_rows(slot) || !transaction.commit()) {
        report_refusal("delete_character", "commit");
        return false;
    }
    selectionMoved = wasSelected;
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=delete_character result=ok soid=0x%016llX chars=%zu selected=%d",
                      static_cast<unsigned long long>(characterSoid),
                      candidate.characterCount,
                      wasSelected ? 1 : 0);
    return true;
}

} // namespace sunrise::state
