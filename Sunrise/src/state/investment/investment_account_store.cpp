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
                          character.nextInventorySerial)
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
    Statement rows("INSERT INTO characters VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
                        character.nextInventorySerial)) {
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
    if (!account::valid_authored(output)) {
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
    if (!account::valid_authored(value)) {
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
    return true;
}

/** A sign-in timestamp belongs only to the current connection lifetime. */
void set_sign_in_time(std::uint64_t seconds) noexcept {
    const std::lock_guard lock(g_mutex);
    g_session.signInSeconds = seconds;
}

} // namespace sunrise::state::investment::store
