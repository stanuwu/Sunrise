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

namespace sunrise::state {
namespace {

namespace objectives = build_data::objectives;
namespace pursuits = build_data::pursuits;
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
    pursuits::Definition contract{};
};

/**
 * Names the saved bank the evaluator reads for one flag or value instruction, as
 * reward_resolver's readers do. Class flags, external values, constants and operators are not
 * saved rows.
 */
[[nodiscard]] bool saved_row(const unlocks::Instruction& instruction, store::Bank& bank) noexcept {
    if (!unlocks::valid(instruction)) {
        return false;
    }
    const bool flag = instruction.opcode == unlocks::Opcode::flag;
    if (!flag && instruction.opcode != unlocks::Opcode::loadValue) {
        return false;
    }
    switch (instruction.bank) {
    case unlocks::Bank::account:
        bank = flag ? store::Bank::accountFlags : store::Bank::objectiveValues;
        return true;
    case unlocks::Bank::profile:
        bank = store::Bank::profileFlags;
        return flag;
    case unlocks::Bank::character:
        bank = flag ? store::Bank::characterObjectFlags : store::Bank::characterObjectValues;
        return true;
    case unlocks::Bank::none:
    case unlocks::Bank::characterClass:
    case unlocks::Bank::external:
        break;
    }
    return false;
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
    const bool copyHeld =
        std::any_of(items.begin(), items.end(), [&identity](const account::inventory::Item& row) {
            return row.instanceSoid == identity.instanceSoid
                   && row.definitionHash == identity.definitionHash
                   && row.mutationSerial == identity.mutationSerial;
        });
    build_data::items::Definition item{};
    if (!copyHeld || !build_data::find_item_definition_hash(identity.definitionHash, item)
        || !build_data::items::pursuit_bucket_item(item)) {
        return false;
    }
    reason = "pursuit_contract";
    if (!build_data::find_pursuit_definition(item.definitionIndex, held.contract)
        || !held.contract.readable || held.contract.definitionHash != identity.definitionHash
        || held.contract.objectiveCount > pursuits::kObjectiveCapacity) {
        return false;
    }
    // No bounty deadline is saved, so an expired bounty looks live and no bounty is edited.
    reason = "expiry_not_saved";
    return held.contract.lifetimeSeconds <= 0;
}

/** @return The contract evaluated against the banks as the savepoint holds them. */
[[nodiscard]] PursuitCompletionStatus inspect_held(const HeldPursuit& held) noexcept {
    return inspect_pursuit_completion(held.contract, held.saved.context());
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
    objectives::Definition objective{};
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
    store::Bank bank{};
    if (objective.expression.size() != 1 || !saved_row(objective.expression.front(), bank)) {
        return false;
    }
    const unlocks::Instruction& source = objective.expression.front();
    const std::int32_t after =
        source.opcode == unlocks::Opcode::flag ? unlocks::kFlagSet : progress.target;
    const auto row = static_cast<std::uint16_t>(source.operand);
    reason = "transaction";
    std::int32_t before = 0;
    if (!store::read_unlock(bank, row, before)) {
        return false;
    }
    if (before == after) {
        return true;
    }
    if (!store::write_unlock(bank, row, after)
        || !store::read_unlocks(held.saved.banks, static_cast<int>(held.saved.slot))) {
        return false;
    }
    change.accountRows = change.accountRows || account_row(bank);
    status = inspect_held(held);
    return true;
}

/**
 * Resolves the single saved row read by the objective the editor named.
 * @param source Receives the objective's only instruction.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool objective_row(const HeldPursuit& held,
                                 std::uint8_t ordinal,
                                 std::uint32_t objectiveHash,
                                 unlocks::Instruction& source,
                                 store::Bank& bank,
                                 const char*& reason) noexcept {
    reason = "objective_changed";
    objectives::Definition objective{};
    if (ordinal >= held.contract.objectiveCount
        || !build_data::find_objective_definition(held.contract.objectives[ordinal], objective)
        || objective.definitionHash != objectiveHash) {
        return false;
    }
    reason = "objective_source";
    if (objective.expression.size() != 1) {
        return false;
    }
    source = objective.expression.front();
    return saved_row(source, bank);
}

/** @return True when every objective that is not optional reads complete. */
[[nodiscard]] bool required_objectives_complete(const HeldPursuit& held,
                                                const PursuitCompletionStatus& status) noexcept {
    for (std::size_t ordinal = 0; ordinal < held.contract.objectiveCount; ++ordinal) {
        objectives::Definition objective{};
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

/** Only an objective whose bound program is one saved flag or value row can be set. */
bool set_pursuit_objective(const PursuitIdentity& identity,
                           std::uint8_t ordinal,
                           std::uint32_t objectiveHash,
                           std::int32_t expected,
                           std::int32_t value,
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
    if (!transaction.ready() || !read_held_pursuit(identity, *held, reason)) {
        return false;
    }
    unlocks::Instruction source{};
    store::Bank bank{};
    if (!objective_row(*held, ordinal, objectiveHash, source, bank, reason)) {
        return false;
    }
    const bool flag = source.opcode == unlocks::Opcode::flag;
    reason = "objective_value";
    if (flag && value != 0 && value != 1) {
        return false;
    }
    reason = "transaction";
    const auto row = static_cast<std::uint16_t>(source.operand);
    std::int32_t before = 0;
    if (!store::read_unlock(bank, row, before)) {
        return false;
    }
    reason = "flag_state";
    if (flag && (before < unlocks::kFlagClear || before > unlocks::kFlagSet)) {
        return false;
    }
    // The evaluator reads a flag as set only at its biased set value.
    const std::int32_t current = flag ? (before == unlocks::kFlagSet ? 1 : 0) : before;
    reason = "value_changed";
    if (current != expected) {
        return false;
    }
    // Every published receipt carries a refresh, so a write that changes nothing is refused.
    reason = "value_unchanged";
    if (current == value) {
        return false;
    }
    reason = "transaction";
    const std::int32_t after =
        flag ? (value != 0 ? unlocks::kFlagSet : unlocks::kFlagClear) : value;
    if (!store::write_unlock(bank, row, after) || !transaction.commit()) {
        return false;
    }
    change = {held->saved.account.primarySoid,
              identity.characterSoid,
              held->saved.slot,
              account_row(bank)};
    reason = nullptr;
    return true;
}

/** Writes in contract order, so each objective is evaluated against the rows already written. */
bool complete_pursuit_objectives(const PursuitIdentity& identity,
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
    if (!transaction.ready() || !read_held_pursuit(identity, *held, reason)) {
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
    reason = "transaction";
    if (!transaction.commit()) {
        return false;
    }
    change = edited;
    reason = nullptr;
    return true;
}

} // namespace sunrise::state
