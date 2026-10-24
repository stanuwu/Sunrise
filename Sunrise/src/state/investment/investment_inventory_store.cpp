#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** Location zero names equipment; location one names the unequipped ordered list. */
constexpr int kEquipmentLocation = 0;
constexpr int kInventoryLocation = 1;

/** Reads each item into its bounded owner and location. */
bool read_items(AccountState& output) noexcept {
    Statement rows("SELECT * FROM items ORDER BY character_slot,location,position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t owner = 0;
        int location = 0;
        std::size_t position = 0;
        account::inventory::Item item;
        if (!rows.columns(owner,
                          location,
                          position,
                          item.instanceSoid,
                          item.definitionHash,
                          item.level,
                          item.quantity,
                          item.mutationSerial,
                          item.flags,
                          item.sockets.policy,
                          item.sockets.plugCount,
                          item.movementAbilityEntry,
                          item.grenadeAbilityEntry,
                          item.superAbilityEntry,
                          item.meleeAbilityEntry,
                          item.classAbilityEntry,
                          item.seen,
                          item.placement)
            || owner >= output.characterCount) {
            return false;
        }
        auto& character = output.characters[owner];
        if (location == kEquipmentLocation && position < character.equipment.slots.size()) {
            character.equipment.slots[position] = item;
        } else if (location == kInventoryLocation && position == character.inventory.count
                   && position < character.inventory.values.size()) {
            character.inventory.values[character.inventory.count++] = item;
        } else {
            return false;
        }
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Socket joins keep empty lanes distinct from the native-default socket policy. */
bool read_sockets(AccountState& output) noexcept {
    Statement rows("SELECT i.character_slot,i.location,i.position,s.lane,s.plug_hash "
                   "FROM sockets s JOIN items i USING(instance_soid)");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t owner = 0;
        int location = 0;
        std::size_t position = 0;
        std::size_t lane = 0;
        std::uint32_t hash = 0;
        if (!rows.columns(owner, location, position, lane, hash)
            || owner >= output.characterCount) {
            return false;
        }
        auto& character = output.characters[owner];
        account::inventory::Item* item = nullptr;
        if (location == kEquipmentLocation && position < character.equipment.slots.size()
            && character.equipment.slots[position]) {
            item = &*character.equipment.slots[position];
        } else if (location == kInventoryLocation && position < character.inventory.count) {
            item = &character.inventory.values[position];
        }
        if (item == nullptr || lane >= item->sockets.plugCount
            || lane >= item->sockets.plugs.size()) {
            return false;
        }
        item->sockets.plugs[lane] = hash;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Profile rows preserve stable instance identities and mutation serials. */
bool read_profile(AccountState& output) noexcept {
    Statement rows("SELECT * FROM profile_items ORDER BY position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t position = 0;
        account::inventory::ProfileItem item;
        if (!rows.columns(position,
                          item.instanceSoid,
                          item.definitionHash,
                          item.quantity,
                          item.mutationSerial,
                          item.seen)
            || position != output.profileItemCount || position >= output.profileItems.size()) {
            return false;
        }
        output.profileItems[output.profileItemCount++] = item;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Character materials use their own ordered list, including runtime grants. */
bool read_stacks(AccountState& output) noexcept {
    Statement rows("SELECT * FROM character_stacks ORDER BY character_slot,position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t owner = 0;
        std::size_t position = 0;
        account::inventory::CharacterStack item;
        if (!rows.columns(owner, position, item.definitionHash, item.quantity, item.mutationSerial)
            || owner >= output.characterCount) {
            return false;
        }
        auto& stacks = output.characters[owner].stacks;
        if (position != stacks.count || position >= stacks.values.size()) {
            return false;
        }
        stacks.values[stacks.count++] = item;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** All socket and ability fields belong to the item instance being saved. */
bool write_item(Statement& items,
                Statement& sockets,
                std::size_t owner,
                int location,
                std::size_t position,
                const account::inventory::Item& item) noexcept {
    if (!items.write(owner,
                     location,
                     position,
                     item.instanceSoid,
                     item.definitionHash,
                     item.level,
                     item.quantity,
                     item.mutationSerial,
                     item.flags,
                     item.sockets.policy,
                     item.sockets.plugCount,
                     item.movementAbilityEntry,
                     item.grenadeAbilityEntry,
                     item.superAbilityEntry,
                     item.meleeAbilityEntry,
                     item.classAbilityEntry,
                     item.seen,
                     item.placement)) {
        return false;
    }
    for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
        if (item.sockets.plugs[lane]
            && !sockets.write(item.instanceSoid, lane, *item.sockets.plugs[lane])) {
            return false;
        }
    }
    return true;
}

} // namespace

/** The caller owns the database transaction and the empty account output. */
bool read_inventory(AccountState& output) noexcept {
    return read_items(output) && read_sockets(output) && read_profile(output)
           && read_stacks(output);
}

/** Inventory rows are replaced inside the account's transaction. */
bool write_inventory(const AccountState& value) noexcept {
    Statement items("INSERT INTO items VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Statement sockets("INSERT INTO sockets VALUES (?,?,?)");
    Statement stacks("INSERT INTO character_stacks VALUES (?,?,?,?,?)");
    for (std::size_t owner = 0; owner < value.characterCount; ++owner) {
        const auto& character = value.characters[owner];
        for (std::size_t position = 0; position < character.equipment.slots.size(); ++position) {
            const auto& item = character.equipment.slots[position];
            if (item && !write_item(items, sockets, owner, kEquipmentLocation, position, *item)) {
                return false;
            }
        }
        for (std::size_t position = 0; position < character.inventory.count; ++position) {
            if (!write_item(items,
                            sockets,
                            owner,
                            kInventoryLocation,
                            position,
                            character.inventory.values[position])) {
                return false;
            }
        }
        for (std::size_t position = 0; position < character.stacks.count; ++position) {
            const auto& item = character.stacks.values[position];
            if (!stacks.write(
                    owner, position, item.definitionHash, item.quantity, item.mutationSerial)) {
                return false;
            }
        }
    }
    Statement profile("INSERT INTO profile_items VALUES (?,?,?,?,?,?)");
    for (std::size_t position = 0; position < value.profileItemCount; ++position) {
        const auto& item = value.profileItems[position];
        if (!profile.write(position,
                           item.instanceSoid,
                           item.definitionHash,
                           item.quantity,
                           item.mutationSerial,
                           item.seen)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::state::investment::store
