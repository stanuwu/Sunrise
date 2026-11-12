#include "pursuit_edit.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/definition.h"
#include "../unlocks/unlocks_expression.h"
#include "pursuit_completion_runtime.h"
#include "saved_selection_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

namespace objective_data = build_data::objectives;
namespace pursuit_data = build_data::pursuits;
namespace store = investment::store;

// The store addresses a row with 16 bits, so a valid operand of every saved bank narrows exactly.
static_assert((std::max)({unlocks::kAccountFlagCapacity,
                          unlocks::kProfileFlagCapacity,
                          unlocks::kCharacterObjectFlagCapacity,
                          unlocks::kObjectiveValueCapacity,
                          unlocks::kCharacterObjectValueCapacity})
              <= (std::numeric_limits<std::uint16_t>::max)());

/** The saved view one pursuit edit reads, and the installed contract of the edited pursuit. */
struct HeldPursuit {
    SavedSelection<AccountState> saved{};
    pursuit_data::Definition contract{};
    /** The edited copy's row in the selected character's inventory. */
    std::size_t itemRow{};
    /** One of the copy's per-item values changed and the account is written before commit. */
    bool itemValuesChanged{};

    [[nodiscard]] account::inventory::Item& item() noexcept {
        return saved.account.characters[saved.slot].inventory.values[itemRow];
    }
    [[nodiscard]] const account::inventory::Item& item() const noexcept {
        return saved.account.characters[saved.slot].inventory.values[itemRow];
    }
    /** @return The copy's per-item values; a copy that never saved any reads zero in every lane. */
    [[nodiscard]] account::inventory::ItemObjectives item_values() const noexcept {
        return item().objectives.value_or(account::inventory::ItemObjectives{});
    }
};

/** Where one objective's progress is saved. */
struct ProgressRow {
    /** Set for progress kept in the held copy's own values, which then names the lane. */
    bool itemHeld{};
    std::size_t lane{};
    /** The unlock bank and row read for any other progress. */
    store::Bank bank{};
    std::uint16_t row{};
    bool flag{};
};

/**
 * Names the saved bank the evaluator reads for one flag or value instruction. Class flags,
 * external values, constants and operators are not saved rows.
 */
[[nodiscard]] bool saved_row(const unlocks::Instruction& instruction, store::Bank& bank) noexcept {
    if (!unlocks::valid(instruction)) {
        return false;
    }
    const bool flag = instruction.opcode == unlocks::Opcode::flag;
    if (!flag && instruction.opcode != unlocks::Opcode::loadValue) {
        return false;
    }
    return store::saved_bank(instruction.bank, flag, bank);
}

/** @return True for banks the account object publishes, false for the character object's. */
[[nodiscard]] bool account_row(store::Bank bank) noexcept {
    switch (bank) {
    case store::Bank::accountFlags:
    case store::Bank::profileFlags:
    case store::Bank::characterFlags:
    case store::Bank::objectiveValues:
    case store::Bank::accountProgressions:
        return true;
    case store::Bank::characterObjectFlags:
    case store::Bank::characterObjectValues:
    case store::Bank::characterProgressions:
        break;
    }
    return false;
}

/**
 * Copies the selected character's saved view inside the caller's savepoint, then finds the
 * identified pursuit and its installed contract.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool read_held_pursuit(const PursuitIdentity& identity,
                                     std::int64_t nowSeconds,
                                     HeldPursuit& held,
                                     const char*& reason) noexcept {
    reason = "not_ready";
    if (!build_data::item_definitions_ready() || !build_data::objective_definitions_ready()
        || !build_data::pursuit_definitions_ready()) {
        return false;
    }
    switch (read_saved_selection(held.saved)) {
    case SavedSelectionRead::read:
        break;
    case SavedSelectionRead::accountInvalid:
        reason = "account_state";
        return false;
    case SavedSelectionRead::unselected:
        reason = "character_changed";
        return false;
    case SavedSelectionRead::accountStorage:
    case SavedSelectionRead::bankStorage:
        reason = "transaction";
        return false;
    }
    const CharacterState& character = held.saved.account.characters[held.saved.slot];
    reason = "character_changed";
    if (character.soid != identity.characterSoid) {
        return false;
    }
    reason = "pursuit_changed";
    const auto items =
        std::span(character.inventory.values)
            .first((std::min)(character.inventory.count, character.inventory.values.size()));
    const auto copy =
        std::find_if(items.begin(), items.end(), [&identity](const account::inventory::Item& row) {
            return row.instanceSoid == identity.instanceSoid
                   && row.definitionHash == identity.definitionHash
                   && row.mutationSerial == identity.mutationSerial;
        });
    held.itemRow = static_cast<std::size_t>(copy - items.begin());
    build_data::items::Definition item{};
    if (copy == items.end() || !build_data::find_item_definition_hash(identity.definitionHash, item)
        || !build_data::items::pursuit_bucket_item(item)) {
        return false;
    }
    reason = "pursuit_contract";
    if (!build_data::find_pursuit_definition(item.definitionIndex, held.contract)
        || !held.contract.readable || held.contract.definitionHash != identity.definitionHash
        || held.contract.objectiveCount > pursuit_data::kObjectiveCapacity) {
        return false;
    }
    // An expired bounty can no longer be redeemed, so progress written to it would never count.
    reason = "expired";
    return !runtime::detail::bounty_expired(held.contract, held.item_values(), nowSeconds);
}

/** @return The contract evaluated against the banks and the held items as edited so far. */
[[nodiscard]] PursuitCompletionStatus inspect_held(const HeldPursuit& held) noexcept {
    const account::inventory::ItemObjectives values = held.item_values();
    const account::inventory::CharacterItems& inventory =
        held.saved.account.characters[held.saved.slot].inventory;
    return inspect_pursuit_completion(held.contract,
                                      held.saved.context(),
                                      values,
                                      std::span(inventory.values).first(inventory.count));
}

/**
 * Finds where one objective's progress is saved: a lane of the held copy's own values when the
 * objective names a value slot the copy carries, otherwise the one saved row its program reads.
 * @return False when the progress is neither, or would be a bounty's deadline lane.
 */
[[nodiscard]] bool progress_row(const HeldPursuit& held,
                                const objective_data::Definition& objective,
                                ProgressRow& output) noexcept {
    output = {};
    if (objective.itemValueSlot != objective_data::kNoItemValueSlot) {
        output.itemHeld = true;
        return runtime::detail::item_value_lane(
            held.contract, objective.itemValueSlot, output.lane);
    }
    if (objective.expression.size() != 1
        || !saved_row(objective.expression.front(), output.bank)) {
        return false;
    }
    const unlocks::Instruction& source = objective.expression.front();
    output.row = static_cast<std::uint16_t>(source.operand);
    output.flag = source.opcode == unlocks::Opcode::flag;
    return true;
}

/**
 * Reads an objective's saved progress as the editor shows it: a flag as 0 or 1.
 * @return False when the row cannot be read or a flag holds a value outside its biased range.
 */
[[nodiscard]] bool read_progress(const HeldPursuit& held,
                                 const ProgressRow& at,
                                 std::int32_t& value,
                                 const char*& reason) noexcept {
    if (at.itemHeld) {
        value = held.item_values()[at.lane];
        return true;
    }
    reason = "transaction";
    std::int32_t saved = 0;
    if (!store::read_unlock(at.bank, at.row, saved)) {
        return false;
    }
    reason = "flag_state";
    if (at.flag && (saved < unlocks::kFlagClear || saved > unlocks::kFlagSet)) {
        return false;
    }
    // The evaluator reads a flag as set only at its biased set value.
    value = at.flag ? (saved == unlocks::kFlagSet ? 1 : 0) : saved;
    return true;
}

/**
 * Writes an objective's progress inside the caller's savepoint. A lane changes the held copy,
 * which reaches the store when the edit commits; a row is written at once and the banks re-read.
 * @param value The editor's value: a flag as 0 or 1.
 */
[[nodiscard]] bool write_progress(HeldPursuit& held,
                                  const ProgressRow& at,
                                  std::int32_t value,
                                  AccountEditChange& change,
                                  const char*& reason) noexcept {
    if (at.itemHeld) {
        account::inventory::ItemObjectives values = held.item_values();
        values[at.lane] = value;
        account::inventory::Item& item = held.item();
        item.objectives = values;
        // A new serial is what tells the client the copy changed, as for any other item mutation.
        if (!held.itemValuesChanged) {
            CharacterState& character = held.saved.account.characters[held.saved.slot];
            reason = "serial";
            if (character.nextInventorySerial
                >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
                return false;
            }
            item.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        }
        held.itemValuesChanged = true;
        change.itemInstanceSoid = item.instanceSoid;
        return true;
    }
    reason = "transaction";
    const std::int32_t saved =
        at.flag ? (value != 0 ? unlocks::kFlagSet : unlocks::kFlagClear) : value;
    if (!store::write_unlock(at.bank, at.row, saved)
        || !store::read_unlocks(held.saved.banks, static_cast<int>(held.saved.slot))) {
        return false;
    }
    change.accountRows = change.accountRows || account_row(at.bank);
    return true;
}

/** Saves the held copy's edited values, then commits the edit's savepoint. */
[[nodiscard]] bool commit_edit(HeldPursuit& held,
                               store::Transaction& transaction,
                               const char*& reason) noexcept {
    reason = "transaction";
    if (held.itemValuesChanged && !store::write_account(held.saved.account)) {
        return false;
    }
    return transaction.commit();
}

/**
 * Writes one required objective's saved row to its adjusted target when the banks, including
 * earlier writes of this savepoint, leave it incomplete. A write re-reads the banks and
 * re-evaluates the contract, so later objectives see the new row.
 * @param status The contract evaluated against the current banks, replaced after a write.
 * @param incomplete Set when the objective was incomplete.
 * @param change Notes an account-object row when the write changed one.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool complete_objective(HeldPursuit& held,
                                      std::size_t ordinal,
                                      PursuitCompletionStatus& status,
                                      bool& incomplete,
                                      AccountEditChange& change,
                                      const char*& reason) noexcept {
    reason = "pursuit_contract";
    objective_data::Definition objective{};
    if (!build_data::find_objective_definition(held.contract.objectives[ordinal], objective)) {
        return false;
    }
    // Optional objectives, skipped ones included, never gate completion.
    if (objective.optional) {
        return true;
    }
    const PursuitObjectiveStatus& progress = status.objectives[ordinal];
    reason = "objective_source";
    if (!progress.known) {
        return false;
    }
    if (progress.complete) {
        return true;
    }
    incomplete = true;
    ProgressRow at{};
    if (!progress_row(held, objective, at)) {
        return false;
    }
    const std::int32_t after = at.flag ? 1 : progress.target;
    std::int32_t before = 0;
    if (!read_progress(held, at, before, reason)) {
        return false;
    }
    if (before == after) {
        return true;
    }
    if (!write_progress(held, at, after, change, reason)) {
        return false;
    }
    status = inspect_held(held);
    return true;
}

/**
 * Resolves where the progress of the objective the editor named is saved.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool objective_row(const HeldPursuit& held,
                                 std::uint8_t ordinal,
                                 std::uint32_t objectiveHash,
                                 ProgressRow& at,
                                 const char*& reason) noexcept {
    reason = "objective_changed";
    objective_data::Definition objective{};
    if (ordinal >= held.contract.objectiveCount
        || !build_data::find_objective_definition(held.contract.objectives[ordinal], objective)
        || objective.definitionHash != objectiveHash) {
        return false;
    }
    reason = "objective_source";
    return progress_row(held, objective, at);
}

/** @return True when every objective that is not optional reads complete. */
[[nodiscard]] bool required_objectives_complete(const HeldPursuit& held,
                                                const PursuitCompletionStatus& status) noexcept {
    for (std::size_t ordinal = 0; ordinal < held.contract.objectiveCount; ++ordinal) {
        objective_data::Definition objective{};
        if (!build_data::find_objective_definition(held.contract.objectives[ordinal], objective)) {
            return false;
        }
        if (!objective.optional && !status.objectives[ordinal].complete) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Only an objective kept in one of the copy's own values, or read from one saved flag or value
 * row, can be set.
 */
bool set_pursuit_objective(const PursuitIdentity& identity,
                           std::uint8_t ordinal,
                           std::uint32_t objectiveHash,
                           std::int32_t expected,
                           std::int32_t value,
                           std::int64_t nowSeconds,
                           AccountEditChange& change,
                           const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    reason = "allocation";
    const std::unique_ptr<HeldPursuit> held(new (std::nothrow) HeldPursuit{});
    if (!held) {
        return false;
    }
    store::Transaction transaction;
    reason = "transaction";
    if (!transaction.ready() || !read_held_pursuit(identity, nowSeconds, *held, reason)) {
        return false;
    }
    ProgressRow at{};
    if (!objective_row(*held, ordinal, objectiveHash, at, reason)) {
        return false;
    }
    reason = "objective_value";
    if (at.flag && value != 0 && value != 1) {
        return false;
    }
    std::int32_t current = 0;
    if (!read_progress(*held, at, current, reason)) {
        return false;
    }
    reason = "value_changed";
    if (current != expected) {
        return false;
    }
    // Every published receipt carries a refresh, so a write that changes nothing is refused.
    reason = "value_unchanged";
    if (current == value) {
        return false;
    }
    AccountEditChange edited{
        held->saved.account.primarySoid, identity.characterSoid, held->saved.slot};
    if (!write_progress(*held, at, value, edited, reason)
        || !commit_edit(*held, transaction, reason)) {
        return false;
    }
    change = edited;
    reason = nullptr;
    return true;
}

/** Writes in contract order, so each objective is evaluated against the rows already written. */
bool complete_pursuit_objectives(const PursuitIdentity& identity,
                                 std::int64_t nowSeconds,
                                 AccountEditChange& change,
                                 const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    change = {};
    reason = "allocation";
    const std::unique_ptr<HeldPursuit> held(new (std::nothrow) HeldPursuit{});
    if (!held) {
        return false;
    }
    store::Transaction transaction;
    reason = "transaction";
    if (!transaction.ready() || !read_held_pursuit(identity, nowSeconds, *held, reason)) {
        return false;
    }
    AccountEditChange edited{
        held->saved.account.primarySoid, identity.characterSoid, held->saved.slot};
    PursuitCompletionStatus status = inspect_held(*held);
    bool incomplete = false;
    for (std::size_t ordinal = 0; ordinal < held->contract.objectiveCount; ++ordinal) {
        if (!complete_objective(*held, ordinal, status, incomplete, edited, reason)) {
            return false;
        }
    }
    reason = "objectives_complete";
    if (!incomplete) {
        return false;
    }
    // A zero target, a flag target other than one, a target adjustment, or a row shared with
    // another objective can still leave an objective incomplete. The savepoint then rolls back
    // every write.
    reason = "objective_target";
    if (!required_objectives_complete(*held, status)) {
        return false;
    }
    if (!commit_edit(*held, transaction, reason)) {
        return false;
    }
    change = edited;
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
