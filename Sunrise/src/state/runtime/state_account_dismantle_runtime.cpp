#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;

namespace {

/**
 * The instance an editor read. Its removal is refused once the character, hash, serial or
 * quantity no longer matches.
 */
struct ExpectedItem {
    std::uint64_t characterSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
    std::int32_t quantity{};
};

/**
 * Checks that the selected character still holds the expected instance, unequipped. Staging
 * would also refuse an equipped row, but this check reports the specific reason.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool holds_expected_item(const CharacterState& character,
                                       std::uint64_t instanceSoid,
                                       const ExpectedItem& expected,
                                       const char*& reason) noexcept {
    reason = "character_changed";
    if (character.soid != expected.characterSoid) {
        return false;
    }
    CharacterItemLocation location{};
    reason = "inventory_row";
    if (instanceSoid == 0 || !find_character_item_location(character, instanceSoid, location)) {
        return false;
    }
    reason = "equipped";
    if (location.equipped) {
        return false;
    }
    reason = "inventory_row";
    const account::inventory::Item* item = character_item_at(character, location);
    return item != nullptr && item->definitionHash == expected.definitionHash
           && item->mutationSerial == expected.mutationSerial
           && item->quantity == expected.quantity;
}

/** The account one removal is staged against, with its selected character. */
struct RemovalSnapshot {
    std::unique_ptr<const AccountState> account;
    std::size_t characterIndex{};
    /** Null when the account is valid and has a selected character, else the refusal label. */
    const char* refusal{};
};

/** Copies the account to the heap and finds its selected character. */
[[nodiscard]] RemovalSnapshot take_removal_snapshot() noexcept {
    RemovalSnapshot snapshot;
    snapshot.account.reset(new (std::nothrow) AccountState(bound_account_snapshot()));
    if (!snapshot.account) {
        snapshot.refusal = "allocation";
    } else if (!account::valid(*snapshot.account)) {
        snapshot.refusal = "account_state";
    } else {
        snapshot.characterIndex = selected_character_index(*snapshot.account);
        if (snapshot.characterIndex >= snapshot.account->characterCount) {
            snapshot.refusal = "character_changed";
        }
    }
    return snapshot;
}

/**
 * Prepares one selected-character unequipped item removal without changing account State.
 * @param paysRewards False stages an editor removal, which credits no dismantle materials.
 * @param expected The instance the caller read, or null to accept whatever the instance holds.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool prepare_selected_item_dismantle(const RemovalSnapshot& snapshot,
                                                   std::uint64_t instanceSoid,
                                                   bool paysRewards,
                                                   const ExpectedItem* expected,
                                                   PendingItemDismantle& mutation,
                                                   const char*& reason) noexcept {
    mutation = {};
    const std::string_view stage = paysRewards ? "prepare" : "prepare_removal";
    const auto fail = [&](const char* label) noexcept {
        reason = label;
        report_dismantle(stage,
                         "fail",
                         label,
                         expected != nullptr ? expected->definitionHash : 0,
                         expected != nullptr ? expected->characterSoid : 0,
                         instanceSoid,
                         0,
                         0,
                         0,
                         0,
                         0);
        return false;
    };
    if (snapshot.refusal != nullptr) {
        return fail(snapshot.refusal);
    }
    const AccountState& account = *snapshot.account;
    const std::size_t characterIndex = snapshot.characterIndex;
    if (expected != nullptr
        && !holds_expected_item(
            account.characters[characterIndex], instanceSoid, *expected, reason)) {
        return fail(reason);
    }
    // Staging applies the in-game dismantle rules, so a locked item or a row the loadout cannot
    // place is refused here too.
    if (!stage_item_dismantle(account, characterIndex, instanceSoid, paysRewards, mutation)) {
        return fail(paysRewards ? "ownership_or_resolve" : "unsupported_removal");
    }
    report_dismantle(stage,
                     "ok",
                     "ready",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     mutation.dismantledInstanceSoid,
                     mutation.inventoryIndex,
                     mutation.inventoryRow,
                     mutation.equipmentSlot,
                     mutation.movedInventoryItemCount,
                     mutation.afterCharacter.nextInventorySerial);
    reason = nullptr;
    return true;
}

/**
 * Captures the consumed item's acquisition flag when its action deletes it and the flag is clear.
 * @return False when the saved flag does not read.
 */
[[nodiscard]] bool prepare_acquired_flag(PendingItemDismantle& mutation) noexcept {
    mutation.acquiredFlag = {};
    mutation.setsAcquiredFlag = false;
    build_data::items::Definition item{};
    build_data::rewards::Item reward{};
    if (!build_data::find_item_definition_hash(mutation.dismantledItem.definitionHash, item)
        || !item.deletesOnAction || !build_data::find_reward_item(item.definitionIndex, reward)
        || reward.definitionHash != item.definitionHash
        || reward.acquiredFlag == build_data::rewards::kAbsent) {
        return true;
    }
    std::int32_t before = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::accountFlags, reward.acquiredFlag, before)
        || before < 0 || before > unlocks::kFlagSet) {
        return false;
    }
    mutation.acquiredFlag = {reward.acquiredFlag, static_cast<std::uint8_t>(before)};
    mutation.setsAcquiredFlag = before != unlocks::kFlagSet;
    return true;
}

/**
 * Sets the prepared acquisition flag while it still holds the value preparation read. Call under
 * the store lock, inside the transaction that writes the dismantle.
 */
[[nodiscard]] bool write_acquired_flag(const PendingItemDismantle& mutation) noexcept {
    if (!mutation.setsAcquiredFlag) {
        return true;
    }
    std::int32_t current = 0;
    return investment::store::read_unlock(
               investment::store::Bank::accountFlags, mutation.acquiredFlag.index, current)
           && current == mutation.acquiredFlag.previousValue
           && investment::store::write_unlock(investment::store::Bank::accountFlags,
                                              mutation.acquiredFlag.index,
                                              unlocks::kFlagSet);
}

/** @return True when a profile row carries the instance; a valid account holds it nowhere else. */
[[nodiscard]] bool profile_holds_instance(const AccountState& account,
                                          std::uint64_t instanceSoid) noexcept {
    const auto rows = std::span(account.profileItems).first(account.profileItemCount);
    return instanceSoid != 0
           && std::any_of(rows.begin(), rows.end(), [instanceSoid](const auto& row) noexcept {
                  return row.instanceSoid == instanceSoid;
              });
}

/**
 * Prepares the removal of one instanced profile row, which no character inventory holds, without
 * changing account State.
 * @param snapshot A snapshot that carries no refusal.
 */
[[nodiscard]] bool prepare_profile_resident_discard(const RemovalSnapshot& snapshot,
                                                    std::uint64_t instanceSoid,
                                                    PendingItemDismantle& mutation) noexcept {
    const bool staged = stage_profile_resident_discard(
        *snapshot.account, snapshot.characterIndex, instanceSoid, mutation);
    report_dismantle("prepare_profile_resident",
                     staged ? "ok" : "fail",
                     staged ? "ready" : "not_held",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     instanceSoid,
                     mutation.inventoryIndex,
                     0,
                     0,
                     0,
                     0);
    return staged;
}

} // namespace

/** Prepares one unequipped item removal, from the selected character or the profile. */
bool prepare_item_dismantle(std::uint64_t instanceSoid, PendingItemDismantle& mutation) noexcept {
    const RemovalSnapshot snapshot = take_removal_snapshot();
    const char* reason = nullptr;
    const bool profileResident =
        snapshot.refusal == nullptr && profile_holds_instance(*snapshot.account, instanceSoid);
    const bool staged =
        profileResident
            ? prepare_profile_resident_discard(snapshot, instanceSoid, mutation)
            : prepare_selected_item_dismantle(
                  snapshot, instanceSoid, true, nullptr, mutation, reason);
    if (staged && prepare_acquired_flag(mutation)) {
        return true;
    }
    mutation = {};
    return false;
}

/** Prepares a held stack's discard without changing account State. */
bool prepare_stack_discard(std::uint32_t definitionHash,
                           std::int32_t heldQuantity,
                           PendingItemDismantle& mutation) noexcept {
    mutation = {};
    const auto report = [&](std::string_view result, std::string_view reason) noexcept {
        report_dismantle("prepare_stack",
                         result,
                         reason,
                         definitionHash,
                         mutation.characterSoid,
                         0,
                         mutation.inventoryIndex,
                         0,
                         0,
                         0,
                         mutation.afterCharacter.nextInventorySerial);
    };
    const RemovalSnapshot snapshot = take_removal_snapshot();
    if (snapshot.refusal != nullptr) {
        report("fail", snapshot.refusal);
        return false;
    }
    if (!stage_stack_discard(
            *snapshot.account, snapshot.characterIndex, definitionHash, heldQuantity, mutation)) {
        report("fail", "stack_not_held");
        return false;
    }
    if (!prepare_acquired_flag(mutation)) {
        report("fail", "acquired_flag");
        mutation = {};
        return false;
    }
    report("ok", "ready");
    return true;
}

/** Prepares the removal of one exact unequipped instance, crediting no dismantle materials. */
bool prepare_item_removal(std::uint64_t characterSoid,
                          std::uint64_t instanceSoid,
                          std::uint32_t definitionHash,
                          std::int32_t mutationSerial,
                          std::int32_t quantity,
                          PendingItemDismantle& mutation,
                          const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    const ExpectedItem expected{characterSoid, definitionHash, mutationSerial, quantity};
    return prepare_selected_item_dismantle(
        take_removal_snapshot(), instanceSoid, false, &expected, mutation, reason);
}

/** Produces the exact character-and-profile after-image while the captured views remain current. */
bool preview_item_dismantle(const PendingItemDismantle& mutation, AccountState& after) noexcept {
    const AccountState current = bound_account_snapshot();
    const bool ready = materialize_item_dismantle(current, mutation, after);
    report_dismantle("preview",
                     ready ? "ok" : "fail",
                     ready ? "ready" : "stale_or_invalid",
                     mutation.dismantledItem.definitionHash,
                     mutation.characterSoid,
                     mutation.dismantledInstanceSoid,
                     mutation.inventoryIndex,
                     mutation.inventoryRow,
                     mutation.equipmentSlot,
                     mutation.movedInventoryItemCount,
                     mutation.afterCharacter.nextInventorySerial);
    return ready;
}

/** Reads the selected character's banks and applies the flag the dismantle sets. */
bool preview_dismantle_unlocks(const PendingItemDismantle& mutation,
                               unlocks::Table& after) noexcept {
    if (!investment::store::read_unlocks(after, static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    if (!mutation.setsAcquiredFlag) {
        return true;
    }
    const std::uint16_t index = mutation.acquiredFlag.index;
    if (index >= after.accountFlags.size()
        || after.accountFlags[index] != mutation.acquiredFlag.previousValue) {
        return false;
    }
    after.accountFlags[index] = unlocks::kFlagSet;
    return true;
}

/** Commits one prepared dismantle only while its complete account views are unchanged. */
bool commit_item_dismantle(PendingItemDismantle& mutation) noexcept {
    const PendingItemDismantle& prepared = mutation;
    const PendingConsumption consume{mutation};
    const auto fail = [&prepared](std::string_view reason) noexcept {
        report_dismantle("commit",
                         "fail",
                         reason,
                         prepared.dismantledItem.definitionHash,
                         prepared.characterSoid,
                         prepared.dismantledInstanceSoid,
                         prepared.inventoryIndex,
                         prepared.inventoryRow,
                         prepared.equipmentSlot,
                         prepared.movedInventoryItemCount,
                         prepared.afterCharacter.nextInventorySerial);
        return false;
    };

    report_dismantle("commit_begin",
                     "ok",
                     "ready",
                     prepared.dismantledItem.definitionHash,
                     prepared.characterSoid,
                     prepared.dismantledInstanceSoid,
                     prepared.inventoryIndex,
                     prepared.inventoryRow,
                     prepared.equipmentSlot,
                     prepared.movedInventoryItemCount,
                     prepared.afterCharacter.nextInventorySerial);

    investment::store::g_mutex.lock();
    AccountState candidate{};
    const bool ready =
        materialize_item_dismantle(investment::store::account(), prepared, candidate);
    bool written = false;
    if (ready) {
        // The inventory and the acquisition flag commit together or not at all.
        investment::store::Transaction transaction;
        written = transaction.ready() && investment::store::write_account(candidate)
                  && write_acquired_flag(prepared) && transaction.commit();
    }
    investment::store::g_mutex.unlock();

    if (!ready) {
        return fail("stale_or_invalid");
    }
    if (!written) {
        return fail("transaction");
    }

    report_dismantle("commit_end",
                     "ok",
                     "published",
                     prepared.dismantledItem.definitionHash,
                     prepared.characterSoid,
                     prepared.dismantledInstanceSoid,
                     prepared.inventoryIndex,
                     prepared.inventoryRow,
                     prepared.equipmentSlot,
                     prepared.movedInventoryItemCount,
                     prepared.afterCharacter.nextInventorySerial);
    return true;
}

} // namespace sunrise::state
