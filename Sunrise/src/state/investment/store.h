#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../account/account_state.h"
#include "../entitlements/definition.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_expression.h"
#include "investment.h"

namespace sunrise::state::investment::store {

/** Bank identifiers are part of schema version 1. */
enum class Bank : int {
    accountFlags,
    profileFlags,
    characterFlags,
    objectiveValues,
    characterObjectFlags,
    characterObjectValues,
    accountProgressions,
    characterProgressions
};

/**
 * Names the saved bank a bound flag or value instruction reads.
 * @param source The instruction's bank.
 * @param flag True for a flag instruction, false for a value instruction.
 * @param bank Receives the saved bank.
 * @return False for a source no saved bank answers: none, a class flag, an external read, or a
 *         profile value.
 */
[[nodiscard]] constexpr bool saved_bank(unlocks::Bank source, bool flag, Bank& bank) noexcept {
    switch (source) {
    case unlocks::Bank::account:
        bank = flag ? Bank::accountFlags : Bank::objectiveValues;
        return true;
    case unlocks::Bank::profile:
        bank = Bank::profileFlags;
        return flag;
    case unlocks::Bank::character:
        bank = flag ? Bank::characterObjectFlags : Bank::characterObjectValues;
        return true;
    case unlocks::Bank::none:
    case unlocks::Bank::characterClass:
    case unlocks::Bank::external:
        break;
    }
    return false;
}

[[nodiscard]] bool initialize(void* module) noexcept;
[[nodiscard]] bool validate() noexcept;
[[nodiscard]] bool open(std::string_view path,
                        std::string_view schema,
                        std::string_view defaults,
                        std::string_view settingsSchema,
                        std::string_view settingsDefaults,
                        std::string_view characterStarter) noexcept;
void shutdown() noexcept;
/** Binds an upstream seed account once, preserving all slot-owned investment rows. */
[[nodiscard]] bool bind_identity(std::uint64_t primarySoid) noexcept;
/** Matches a persisted account or character root; requires private local access. */
[[nodiscard]] bool owns_account_root(std::uint64_t rootSoid) noexcept;
/**
 * Reads real character slots and the session selection under one private-local transaction.
 * Failure clears output; success returns zero when no existing character is selected.
 */
[[nodiscard]] bool read_selected_character(std::uint64_t& output) noexcept;
/**
 * Moves the session to an in-memory copy of the open save and closes the file, so no later
 * write reaches it. The copy is gone at shutdown. Call it before any thread uses the store.
 * @return False when the copy failed; the file handle then stays open and unchanged.
 */
[[nodiscard]] bool use_session_copy() noexcept;
/** @return A count that moves on every committed write and selection change. */
[[nodiscard]] std::uint64_t revision() noexcept;
[[nodiscard]] bool read_account(AccountState& output) noexcept;
[[nodiscard]] AccountState account() noexcept;
[[nodiscard]] bool read_account_identity(AccountIdentity& output) noexcept;
[[nodiscard]] bool write_account(const AccountState& value) noexcept;
[[nodiscard]] bool read_settings(account::settings::AccountSettings& output) noexcept;
[[nodiscard]] bool write_settings(const account::settings::AccountSettings& value) noexcept;
void set_sign_in_time(std::uint64_t seconds) noexcept;
[[nodiscard]] bool read_family5(Family5State& output) noexcept;
[[nodiscard]] bool write_family5(const Family5State& value) noexcept;
[[nodiscard]] bool read_unlocks(unlocks::Table& output, int characterSlot = -1) noexcept;
[[nodiscard]] bool write_unlocks(const unlocks::Table& value, int characterSlot = -1) noexcept;
[[nodiscard]] bool read_unlock(Bank bank, std::uint16_t slot, std::int32_t& value) noexcept;
[[nodiscard]] bool write_unlock(Bank bank, std::uint16_t slot, std::int32_t value) noexcept;
[[nodiscard]] bool read_entitlements(entitlements::Table& output) noexcept;
/**
 * Reads the generation a Store wallet sync last committed.
 * @param revision Receives the revision, or zero when the account never synced.
 */
[[nodiscard]] bool read_store_wallet_sync(std::uint32_t& revision) noexcept;
/**
 * Advances the Store wallet sync generation.
 * @param revision Receives the new revision; zero on failure.
 */
[[nodiscard]] bool advance_store_wallet_sync(std::uint32_t& revision) noexcept;
[[nodiscard]] bool bootstrap_completed(std::string_view name) noexcept;
[[nodiscard]] bool complete_bootstrap(std::string_view name) noexcept;

/**
 * Fills a created character's level, card, destination and content fields from the save.
 * @param character Character whose other fields stay unchanged.
 * @return False when the one starter row does not read.
 */
[[nodiscard]] bool read_starter_character(CharacterState& character) noexcept;

/**
 * Reads Sunrise's own starter rows for one class, in position order.
 * @param characterClass Class byte: 0 Titan, 1 Hunter, 2 Warlock.
 * @param output Receives the definition hashes.
 * @param count Receives the row count.
 * @return False when the rows do not read or do not fit output.
 */
[[nodiscard]] bool read_starter_items(std::uint8_t characterClass,
                                      std::span<std::uint32_t> output,
                                      std::size_t& count) noexcept;

/**
 * Gives a created character the starter unlock rows, the set every default character starts with.
 * @param slot Character slot the new character holds; it has no unlock rows yet.
 * @return False when a row does not copy.
 */
[[nodiscard]] bool seed_character_unlocks(std::size_t slot) noexcept;

/**
 * Drops one character's slot-keyed unlock, reward and parked rows and moves later slots down one.
 * @param slot Character slot the removed character held.
 * @return True when every row moved in one transaction.
 */
[[nodiscard]] bool remove_character_rows(std::size_t slot) noexcept;

/** An earned reward stays in the database until its inventory grant commits. */
struct PendingReward {
    std::uint64_t id{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
};
[[nodiscard]] bool enqueue_reward(std::uint32_t definitionHash, std::int32_t quantity) noexcept;
/**
 * Reads the selected character's oldest saved reward after one row.
 * @param afterId Row id to read past; zero starts at the oldest reward.
 * @param readFailed Receives true when no row was returned because the read failed.
 */
[[nodiscard]] bool
next_reward(PendingReward& output, std::uint64_t afterId, bool& readFailed) noexcept;
[[nodiscard]] bool complete_reward(std::uint64_t id) noexcept;

/** Which account list a parked row came from; the values are the parked_items kind column. */
enum class ParkedKind : std::uint8_t {
    item = 0,
    stack = 1,
    profile = 2,
};

/**
 * One row set aside because the installed content lacks its definition. A stack keeps only its
 * hash, quantity and serial in `item`; a profile row also keeps its identity and seen state.
 */
struct ParkedRow {
    std::int64_t id{};
    ParkedKind kind{};
    /** -1 for a profile row. */
    int characterSlot{};
    /** 0 equipment, 1 the unequipped list; stacks and profile rows use 1. */
    int location{};
    std::size_t position{};
    account::inventory::Item item;
};

/** Adds one parked row; `row.id` is ignored. */
[[nodiscard]] bool park(const ParkedRow& row) noexcept;
[[nodiscard]] bool read_parked(std::vector<ParkedRow>& output) noexcept;
/** Drops one parked row after its content is back in the account. */
[[nodiscard]] bool unpark(std::int64_t id) noexcept;

} // namespace sunrise::state::investment::store
