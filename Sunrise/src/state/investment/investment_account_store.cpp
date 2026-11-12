#include "../account/account_context.h"
#include "../account/public_profiles.h"
#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

// character_helmet_tiers has one column per role, read and written here in GambitPrimeRole order.
static_assert(kGambitPrimeRoleCount == 4);

/**
 * Reads characters in stable slot order and overlays only process-local session fields. A
 * character with no helmet row has earned no tier.
 */
bool read_characters(AccountState& output) noexcept {
    Statement rows("SELECT c.*,COALESCE(h.reaper,0),COALESCE(h.invader,0),"
                   "COALESCE(h.collector,0),COALESCE(h.sentry,0) FROM characters c "
                   "LEFT JOIN character_helmet_tiers h ON h.character_slot=c.slot ORDER BY c.slot");
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
                          character.gambitPrimeHelmetTiers[0],
                          character.gambitPrimeHelmetTiers[1],
                          character.gambitPrimeHelmetTiers[2],
                          character.gambitPrimeHelmetTiers[3])
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
    Statement rows("INSERT INTO characters VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
                        character.appearanceHeader)) {
            return false;
        }
    }
    Statement helmets("INSERT INTO character_helmet_tiers VALUES (?,?,?,?,?)");
    for (std::size_t slot = 0; slot < value.characterCount; ++slot) {
        const auto& tiers = value.characters[slot].gambitPrimeHelmetTiers;
        if (!helmets.write(slot, tiers[0], tiers[1], tiers[2], tiers[3])) {
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

/** Reads only the keys and classes, in slot order, and overlays the session selection. */
bool read_account_identity(AccountIdentity& output) noexcept {
    output = {};
    Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    Statement account("SELECT soid FROM account WHERE id=1");
    if (account.step() != SQLITE_ROW || !account.columns(output.primarySoid)
        || account.step() != SQLITE_DONE) {
        output = {};
        return false;
    }
    Statement rows("SELECT slot,soid,class FROM characters ORDER BY slot");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t slot = 0;
        CharacterIdentity character;
        if (!rows.columns(slot, character.soid, character.characterClass)
            || slot != output.characterCount || slot >= output.characters.size()) {
            output = {};
            return false;
        }
        character.selected = g_session.selected[slot];
        output.characters[output.characterCount++] = character;
        result = rows.step();
    }
    if (result != SQLITE_DONE || !transaction.commit()) {
        output = {};
        return false;
    }
    return true;
}

/** The session overlay changes only after every durable account row commits. */
bool write_account(const AccountState& value) noexcept {
    if (!account::valid(value)) {
        return false;
    }
    Transaction transaction;
    if (!transaction.ready()
        || !execute("DELETE FROM items; DELETE FROM character_stacks;"
                    "DELETE FROM character_helmet_tiers; DELETE FROM characters;"
                    "DELETE FROM profile_items; DELETE FROM dismantle_rewards;")) {
        return false;
    }
    Statement accountRow("INSERT OR REPLACE INTO account VALUES (1,?,?)");
    if (!accountRow.write(value.primarySoid, value.profileSetupCompleted)
        || !write_characters(value)
        || !execute("DELETE FROM character_objective_values "
                    "WHERE character_soid NOT IN (SELECT soid FROM characters)")
        || !write_inventory(value) || !write_settings(value.settings) || !transaction.commit()) {
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

/** One INSERT copies the starter rows under the new slot. */
bool seed_character_unlocks(std::size_t slot) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement rows("INSERT INTO unlocks SELECT ?,bank,slot,lane,value FROM starter_unlocks");
    return rows.write(slot);
}

/** Deletes and moves share one transaction, so a refusal leaves every row in place. */
bool remove_character_rows(std::size_t slot) noexcept {
    Transaction transaction;
    Statement unlocks("DELETE FROM unlocks WHERE character_slot=?");
    Statement rewards("DELETE FROM pending_rewards WHERE character_slot=?");
    Statement parked("DELETE FROM parked_items WHERE character_slot=?");
    if (!transaction.ready() || slot >= kCharacterCapacity || !unlocks.write(slot)
        || !rewards.write(slot) || !parked.write(slot)) {
        return false;
    }
    Statement moveUnlocks("UPDATE unlocks SET character_slot=? WHERE character_slot=?");
    Statement moveRewards("UPDATE pending_rewards SET character_slot=? WHERE character_slot=?");
    Statement moveParked("UPDATE parked_items SET character_slot=? WHERE character_slot=?");
    // Ascending, so every target slot is already empty and no key collides.
    for (std::size_t from = slot + 1; from < kCharacterCapacity; ++from) {
        if (!moveUnlocks.write(from - 1, from) || !moveRewards.write(from - 1, from)
            || !moveParked.write(from - 1, from)) {
            return false;
        }
    }
    return transaction.commit();
}

/** Reads the saved account's Store wallet sync revision; one that never synced reads zero. */
bool read_store_wallet_sync(std::uint32_t& revision) noexcept {
    revision = 0;
    Transaction transaction;
    Statement row("SELECT revision FROM eververse_wallet_sync "
                  "WHERE account_soid=(SELECT soid FROM account WHERE id=1)");
    if (!transaction.ready()) {
        return false;
    }
    const int result = row.step();
    if (result == SQLITE_ROW
        && (!row.column(0, revision) || revision == 0 || row.step() != SQLITE_DONE)) {
        return false;
    }
    return (result == SQLITE_ROW || result == SQLITE_DONE) && transaction.commit();
}

/** Advances the saved account's Store wallet sync revision by one. */
bool advance_store_wallet_sync(std::uint32_t& revision) noexcept {
    revision = 0;
    Transaction transaction;
    std::uint32_t before = 0;
    if (!transaction.ready() || !read_store_wallet_sync(before)
        || before == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    Statement row("INSERT INTO eververse_wallet_sync(account_soid,revision) "
                  "SELECT soid,? FROM account WHERE id=1 "
                  "ON CONFLICT(account_soid) DO UPDATE SET revision=excluded.revision");
    if (!row.write(before + 1U) || !transaction.commit()) {
        return false;
    }
    revision = before + 1U;
    return true;
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
