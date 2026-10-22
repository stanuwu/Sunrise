#include "../account/account_context.h"
#include "../account/public_profiles.h"
#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** Reads characters in stable slot order and overlays only process-local session fields. */
bool read_characters(AccountState& output) noexcept {
    Statement rows("SELECT * FROM characters ORDER BY slot");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t slot = 0;
        CharacterState character;
        if (!rows.columns(slot,
                          character.soid,
                          character.race,
                          character.gender,
                          character.characterClass,
                          character.level,
                          character.previewAvailable,
                          character.appearanceValue,
                          character.lastOrbitedDestination,
                          character.contentBypass,
                          character.equippedTitleRecordIndex,
                          character.acquiredSubclassAbilityMask,
                          character.nextInventorySerial,
                          character.appearanceHeader,
                          character.appearanceHeaderValid)
            || slot != output.characterCount || slot >= output.characters.size()) {
            return false;
        }
        character.selected = g_session.selected[slot];
        character.currentActivityIndex = g_session.activities[slot];
        character.signInSeconds = g_session.signInSeconds;
        output.characters[output.characterCount++] = character;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Economy rows keep their authored order and bounded capacity. */
bool read_rewards(AccountState& output) noexcept {
    Statement rows("SELECT * FROM dismantle_rewards ORDER BY position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t position = 0;
        DismantleRewardPolicy reward;
        if (!rows.columns(position,
                          reward.definitionHash,
                          reward.quantity,
                          reward.tierMask,
                          reward.classMask,
                          reward.masterwork)
            || position != output.dismantleRewardCount
            || position >= output.dismantleRewards.size()) {
            return false;
        }
        output.dismantleRewards[output.dismantleRewardCount++] = reward;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Characters are saved before their items to satisfy foreign keys. */
bool write_characters(const AccountState& value) noexcept {
    Statement rows("INSERT INTO characters VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (std::size_t slot = 0; slot < value.characterCount; ++slot) {
        const auto& character = value.characters[slot];
        if (!rows.write(slot,
                        character.soid,
                        character.race,
                        character.gender,
                        character.characterClass,
                        character.level,
                        character.previewAvailable,
                        character.appearanceValue,
                        character.lastOrbitedDestination,
                        character.contentBypass,
                        character.equippedTitleRecordIndex,
                        character.acquiredSubclassAbilityMask,
                        character.nextInventorySerial,
                        character.appearanceHeader,
                        character.appearanceHeaderValid)) {
            return false;
        }
    }
    Statement rewards("INSERT INTO dismantle_rewards VALUES (?,?,?,?,?,?)");
    for (std::size_t index = 0; index < value.dismantleRewardCount; ++index) {
        const auto& reward = value.dismantleRewards[index];
        if (!rewards.write(index,
                           reward.definitionHash,
                           reward.quantity,
                           reward.tierMask,
                           reward.classMask,
                           reward.masterwork)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool owns_account_root(std::uint64_t rootSoid) noexcept {
    const std::lock_guard lock(g_mutex);
    if (!local_account_access() || rootSoid == 0) {
        return false;
    }
    // One scalar query avoids loading equipment, preferences and inventory just to route a root.
    Statement row("SELECT EXISTS(SELECT 1 FROM account WHERE id=1 AND soid=?1 "
                  "UNION ALL SELECT 1 FROM characters WHERE soid=?1)");
    int owned{};
    return row.parameters(rootSoid) && row.step() == SQLITE_ROW && row.column(0, owned)
           && row.step() == SQLITE_DONE && owned != 0;
}

bool read_selected_character(std::uint64_t& output) noexcept {
    output = 0;
    if (!local_account_access()) {
        return false;
    }
    Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    Statement owner("SELECT soid FROM account WHERE id=1");
    std::uint64_t primary{};
    if (owner.step() != SQLITE_ROW || !owner.column(0, primary) || primary == 0
        || owner.step() != SQLITE_DONE) {
        return false;
    }
    // Selection belongs to the process-local session; SQLite supplies only slot identities.
    Statement rows("SELECT slot,soid FROM characters ORDER BY slot");
    std::size_t count{};
    std::uint64_t selected{};
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t slot{};
        std::uint64_t character{};
        if (!rows.columns(slot, character) || slot != count || slot >= kCharacterCapacity
            || character == 0) {
            return false;
        }
        if (g_session.selected[slot]) {
            if (selected != 0) {
                return false;
            }
            selected = character;
        }
        ++count;
        result = rows.step();
    }
    if (result != SQLITE_DONE || !transaction.commit()) {
        return false;
    }
    output = selected;
    return true;
}

/** A read returns one complete account or an empty output on failure. */
bool read_account(AccountState& output) noexcept {
    Transaction transaction;
    if (!transaction.ready()) {
        output = {};
        return false;
    }
    output = {};
    Statement row("SELECT soid,profile_setup_completed FROM account WHERE id=1");
    if (row.step() != SQLITE_ROW || !row.columns(output.primarySoid, output.profileSetupCompleted)
        || row.step() != SQLITE_DONE || !read_characters(output) || !read_inventory(output)
        || !read_rewards(output)) {
        output = {};
        return false;
    }
    if (!read_settings(output.settings)) {
        output = {};
        return false;
    }
    if (!account::valid(output)) {
        output = {};
        return false;
    }
    return transaction.commit();
}

/** @return A call-local snapshot; a failed database read yields an empty account. */
AccountState account() noexcept {
    AccountState output;
    (void)read_account(output);
    return output;
}

/** The session overlay changes only after every durable account row commits. */
bool write_account(const AccountState& value) noexcept {
    if (!account::valid(value)) {
        return false;
    }
    Transaction transaction;
    if (!transaction.ready()
        || !execute("DELETE FROM items; DELETE FROM character_stacks; DELETE FROM characters;"
                    "DELETE FROM profile_items; DELETE FROM dismantle_rewards;")) {
        return false;
    }
    Statement accountRow("INSERT OR REPLACE INTO account VALUES (1,?,?)");
    if (!accountRow.write(value.primarySoid, value.profileSetupCompleted)
        || !write_characters(value) || !write_inventory(value) || !write_settings(value.settings)
        || !transaction.commit()) {
        return false;
    }
    for (std::size_t index = 0; index < value.characters.size(); ++index) {
        g_session.selected[index] = value.characters[index].selected;
        g_session.activities[index] = value.characters[index].currentActivityIndex;
    }
    account::profiles::local_changed();
    return true;
}

/**
 * Fills a created character's level, card, destination and content fields from the save.
 * @param character Character whose other fields stay unchanged.
 * @return False when the one starter row does not read; the character is then unchanged.
 */
bool read_starter_character(CharacterState& character) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("SELECT level,preview_available,appearance_value,last_orbited_destination,"
                  "content_bypass FROM starter_character WHERE id=1");
    CharacterState staged = character;
    if (row.step() != SQLITE_ROW
        || !row.columns(staged.level,
                        staged.previewAvailable,
                        staged.appearanceValue,
                        staged.lastOrbitedDestination,
                        staged.contentBypass)) {
        return false;
    }
    character = staged;
    return true;
}

/**
 * Reads Sunrise's own starter rows for one class, in position order.
 * @param characterClass Class byte: 0 Titan, 1 Hunter, 2 Warlock.
 * @param output Receives the definition hashes.
 * @param count Receives the row count, zero on failure.
 * @return False when the rows do not read or do not fit output.
 */
bool read_starter_items(std::uint8_t characterClass,
                        std::span<std::uint32_t> output,
                        std::size_t& count) noexcept {
    const std::lock_guard lock(g_mutex);
    count = 0;
    Statement rows("SELECT definition_hash FROM starter_items WHERE class=? ORDER BY position");
    if (!rows.parameters(characterClass)) {
        return false;
    }
    int result = rows.step();
    while (result == SQLITE_ROW) {
        if (count >= output.size() || !rows.column(0, output[count])) {
            count = 0;
            return false;
        }
        ++count;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/**
 * Drops one character's slot-keyed unlock and reward rows and moves later slots down one.
 * @param slot Character slot the removed character held.
 * @return True when every row moved in one transaction.
 */
bool remove_character_rows(std::size_t slot) noexcept {
    Transaction transaction;
    Statement unlocks("DELETE FROM unlocks WHERE character_slot=?");
    Statement rewards("DELETE FROM pending_rewards WHERE character_slot=?");
    if (!transaction.ready() || slot >= kCharacterCapacity || !unlocks.write(slot)
        || !rewards.write(slot)) {
        return false;
    }
    Statement moveUnlocks("UPDATE unlocks SET character_slot=? WHERE character_slot=?");
    Statement moveRewards("UPDATE pending_rewards SET character_slot=? WHERE character_slot=?");
    // Ascending, so every target slot is already empty and no key collides.
    for (std::size_t from = slot + 1; from < kCharacterCapacity; ++from) {
        if (!moveUnlocks.write(from - 1, from) || !moveRewards.write(from - 1, from)) {
            return false;
        }
    }
    return transaction.commit();
}

/** A sign-in timestamp belongs only to the current connection lifetime. */
void set_sign_in_time(std::uint64_t seconds) noexcept {
    const std::lock_guard lock(g_mutex);
    if (!local_account_access()) {
        return;
    }
    if (g_session.signInSeconds == seconds) {
        return;
    }
    g_session.signInSeconds = seconds;
    account::profiles::local_changed();
}

} // namespace sunrise::state::investment::store
