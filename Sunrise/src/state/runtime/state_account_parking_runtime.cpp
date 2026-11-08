/** Parks account rows whose content is not installed, and returns them once it is again. */
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../account/inventory/inventory_lists.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace store = investment::store;
namespace family4_loadout = middleware::datagen::family4::loadout;

namespace {

/** Equipment rows use location 0 and every other list location 1, as the items table does. */
constexpr int kEquipmentLocation = 0;
constexpr int kListLocation = 1;
/** A profile row belongs to no character. */
constexpr int kProfileOwner = -1;

[[nodiscard]] bool installed(std::uint32_t definitionHash) noexcept {
    build_data::items::Definition definition{};
    return build_data::find_item_definition_hash(definitionHash, definition);
}

/** An item needs its own definition and the definition of every plug it carries. */
[[nodiscard]] bool installed(const authored_inventory::Item& item) noexcept {
    if (!installed(item.definitionHash)) {
        return false;
    }
    for (std::size_t lane = 0; lane < item.sockets.plugCount && lane < item.sockets.plugs.size();
         ++lane) {
        if (item.sockets.plugs[lane].has_value() && !installed(*item.sockets.plugs[lane])) {
            return false;
        }
    }
    return true;
}

/** Writes one line about a row that left or rejoined the account. */
void report(core::log::Level level, const char* stage, const store::ParkedRow& row) noexcept {
    core::log::writef(core::log::Channel::state,
                      level,
                      "ev=items stage=%s kind=%u character=%d location=%d position=%zu "
                      "definition=0x%08X soid=0x%016llX",
                      stage,
                      static_cast<unsigned>(row.kind),
                      row.characterSlot,
                      row.location,
                      row.position,
                      row.item.definitionHash,
                      static_cast<unsigned long long>(row.item.instanceSoid));
}

/** @return The next per-character serial, or no value when the counter is spent. */
[[nodiscard]] std::optional<std::int32_t> next_serial(CharacterState& character) noexcept {
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(character.nextInventorySerial++);
}

/**
 * Equips the first unequipped item of the character that fits an emptied slot.
 * @param character Character whose slot was emptied.
 * @param semanticIndex The emptied semantic equipment slot.
 */
void refill(CharacterState& character, std::size_t semanticIndex) noexcept {
    auto& inventory = character.inventory;
    for (std::size_t index = 0; index < inventory.count; ++index) {
        std::uint8_t nativeSlot = 0;
        std::size_t semantic = 0;
        if (!native_equipment_slot(inventory.values[index], nativeSlot)
            || !semantic_equipment_slot(nativeSlot, semantic) || semantic != semanticIndex) {
            continue;
        }
        const std::optional<std::int32_t> serial = next_serial(character);
        if (!serial.has_value()) {
            return;
        }
        authored_inventory::Item moved = inventory.values[index];
        moved.mutationSerial = *serial;
        authored_inventory::erase(inventory.values, inventory.count, index);
        character.equipment.slots[semanticIndex] = moved;
        return;
    }
}

/**
 * Takes every row whose content is not installed out of the account.
 * @param account Account edited in place.
 * @param parked Receives the rows that left, in account order.
 */
void take_uninstalled(AccountState& account, std::vector<store::ParkedRow>& parked) noexcept {
    for (std::size_t owner = 0; owner < account.characterCount; ++owner) {
        CharacterState& character = account.characters[owner];
        const int slot = static_cast<int>(owner);
        authored_inventory::compact(
            character.inventory.values,
            character.inventory.count,
            [](const authored_inventory::Item& item) { return installed(item); },
            [&parked, slot](std::size_t index, const authored_inventory::Item& item) {
                parked.push_back({0, store::ParkedKind::item, slot, kListLocation, index, item});
            });
        // Equipment goes after the list, so a refill only picks an item that stays.
        for (std::size_t index = 0; index < character.equipment.slots.size(); ++index) {
            auto& equipped = character.equipment.slots[index];
            if (equipped.has_value() && !installed(*equipped)) {
                parked.push_back(
                    {0, store::ParkedKind::item, slot, kEquipmentLocation, index, *equipped});
                equipped.reset();
                refill(character, index);
            }
        }
        authored_inventory::compact(
            character.stacks.values,
            character.stacks.count,
            [](const authored_inventory::CharacterStack& stack) {
                return installed(stack.definitionHash);
            },
            [&parked, slot](std::size_t index, const authored_inventory::CharacterStack& stack) {
                store::ParkedRow row{0, store::ParkedKind::stack, slot, kListLocation, index, {}};
                row.item.definitionHash = stack.definitionHash;
                row.item.quantity = stack.quantity;
                row.item.mutationSerial = stack.mutationSerial;
                row.item.placement = stack.placement;
                parked.push_back(row);
            });
    }
    authored_inventory::compact(
        account.profileItems,
        account.profileItemCount,
        [](const authored_inventory::ProfileItem& item) { return installed(item.definitionHash); },
        [&parked](std::size_t index, const authored_inventory::ProfileItem& item) {
            store::ParkedRow row{
                0, store::ParkedKind::profile, kProfileOwner, kListLocation, index, {}};
            row.item.instanceSoid = item.instanceSoid;
            row.item.definitionHash = item.definitionHash;
            row.item.quantity = item.quantity;
            row.item.mutationSerial = item.mutationSerial;
            row.item.seen = item.seen;
            parked.push_back(row);
        });
}

/**
 * Places one parked character item: its old equipped slot when empty, else the end of the list.
 * @return False when the character has no room or no serial left.
 */
[[nodiscard]] bool place_item(AccountState& account, const store::ParkedRow& row) noexcept {
    CharacterState& character = account.characters[static_cast<std::size_t>(row.characterSlot)];
    authored_inventory::Item item = row.item;
    // The allocator never sees parked rows, so a newer item may hold this identity now.
    if (account_owns_soid(account, item.instanceSoid)
        && !next_item_instance_soid(account, item.instanceSoid)) {
        return false;
    }
    const std::optional<std::int32_t> serial = next_serial(character);
    if (!serial.has_value()) {
        return false;
    }
    item.mutationSerial = *serial;
    if (row.location == kEquipmentLocation && row.position < character.equipment.slots.size()
        && !character.equipment.slots[row.position].has_value()) {
        character.equipment.slots[row.position] = item;
        return true;
    }
    if (character.inventory.count >= character.inventory.values.size()) {
        return false;
    }
    character.inventory.values[character.inventory.count++] = item;
    return true;
}

/**
 * Places one parked row of any kind into a trial account.
 * @return False when the row's content is still missing or the row has no room.
 */
[[nodiscard]] bool place(AccountState& account, const store::ParkedRow& row) noexcept {
    const bool character = row.characterSlot >= 0
                           && static_cast<std::size_t>(row.characterSlot) < account.characterCount;
    switch (row.kind) {
    case store::ParkedKind::item:
        return character && installed(row.item) && place_item(account, row);
    case store::ParkedKind::stack: {
        if (!character || !installed(row.item.definitionHash)) {
            return false;
        }
        auto& stacks = account.characters[static_cast<std::size_t>(row.characterSlot)].stacks;
        // A returning stack joins the stack of its definition in the bucket it left.
        for (std::size_t index = 0; index < stacks.count; ++index) {
            auto& stack = stacks.values[index];
            if (stack.definitionHash == row.item.definitionHash
                && stack.placement == row.item.placement) {
                if (stack.quantity
                    > (std::numeric_limits<std::int32_t>::max)() - row.item.quantity) {
                    return false;
                }
                stack.quantity += row.item.quantity;
                return true;
            }
        }
        if (stacks.count >= stacks.values.size()) {
            return false;
        }
        stacks.values[stacks.count++] = {row.item.definitionHash,
                                         row.item.quantity,
                                         row.item.mutationSerial,
                                         row.item.placement};
        return true;
    }
    case store::ParkedKind::profile:
        if (!installed(row.item.definitionHash)
            || account.profileItemCount >= account.profileItems.size()) {
            return false;
        }
        account.profileItems[account.profileItemCount++] = {row.item.instanceSoid,
                                                            row.item.definitionHash,
                                                            row.item.quantity,
                                                            row.item.mutationSerial,
                                                            row.item.seen};
        return true;
    }
    return false;
}

/** Names the step that left the account unchanged; it always returns failed. */
[[nodiscard]] ParkingOutcome fail(const char* step) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=items stage=reconcile result=fail step=%s",
                      step);
    return ParkingOutcome::failed;
}

/** @return True when every character's items still resolve against the installed content. */
[[nodiscard]] bool loadouts_resolve(const AccountState& account) noexcept {
    for (std::size_t owner = 0; owner < account.characterCount; ++owner) {
        family4_loadout::ResolvedInstances instances{};
        if (!family4_loadout::resolve_owned_instances(account, owner, instances)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Parks rows of uninstalled content and returns parked rows whose content is back, in one
 * transaction. A returned row is kept only when the account stays valid and every loadout
 * still resolves; otherwise it stays parked.
 */
ParkingOutcome reconcile_installed_content() noexcept {
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::inventory_bucket_descriptors_ready()
        || !build_data::socket_entry_lists_ready()) {
        return ParkingOutcome::notReady;
    }
    store::Transaction transaction;
    if (!transaction.ready()) {
        return fail("transaction");
    }
    AccountState account = store::account();
    if (!account::valid(account)) {
        return ParkingOutcome::notReady;
    }
    std::vector<store::ParkedRow> waiting;
    if (!store::read_parked(waiting)) {
        return fail("read_parked");
    }
    std::vector<store::ParkedRow> leaving;
    take_uninstalled(account, leaving);
    std::vector<store::ParkedRow> returned;
    for (const store::ParkedRow& row : waiting) {
        AccountState trial = account;
        if (!place(trial, row) || !account::valid(trial) || !loadouts_resolve(trial)) {
            continue;
        }
        if (!store::unpark(row.id)) {
            return fail("unpark");
        }
        account = trial;
        returned.push_back(row);
    }
    for (const store::ParkedRow& row : leaving) {
        if (!store::park(row)) {
            return fail("park");
        }
    }
    const bool changed = !leaving.empty() || !returned.empty();
    if (changed && !account::valid(account)) {
        return fail("account_valid");
    }
    if (changed && !store::write_account(account)) {
        return fail("write_account");
    }
    if (!transaction.commit()) {
        return fail("commit");
    }
    // Reported after the commit, so a line never names a move that rolled back.
    for (const store::ParkedRow& row : returned) {
        report(core::log::Level::info, "return", row);
    }
    for (const store::ParkedRow& row : leaving) {
        report(core::log::Level::warn, "park", row);
    }
    return ParkingOutcome::ready;
}

} // namespace sunrise::state
