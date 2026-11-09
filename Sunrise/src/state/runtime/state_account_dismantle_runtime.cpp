#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
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

/**
 * Prepares one selected-character unequipped item removal without changing account State.
 * @param paysRewards False stages an editor removal, which credits no dismantle materials.
 * @param expected The instance the caller read, or null to accept whatever the instance holds.
 * @param reason Receives the refusal label on failure.
 */
[[nodiscard]] bool prepare_selected_item_dismantle(std::uint64_t instanceSoid,
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
    const std::unique_ptr<const AccountState> snapshot(new (std::nothrow)
                                                           AccountState(account_snapshot()));
    if (!snapshot) {
        return fail("allocation");
    }
    const AccountState& account = *snapshot;
    if (!account::valid(account)) {
        return fail("account_state");
    }
    const std::size_t characterIndex = selected_character_index(account);
    if (characterIndex >= account.characterCount) {
        return fail("character_changed");
    }
    if (expected != nullptr
        && !holds_expected_item(
            account.characters[characterIndex], instanceSoid, *expected, reason)) {
        return fail(reason);
    }
    // Staging applies the in-game dismantle rules, so a locked item or a row the loadout cannot
    // place is refused here too.
    if (!stage_item_dismantle(account, characterIndex, instanceSoid, paysRewards, mutation)) {
        return fail("unsupported_removal");
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

} // namespace

/** Prepares one selected-character unequipped item removal without changing account State. */
bool prepare_item_dismantle(std::uint64_t instanceSoid, PendingItemDismantle& mutation) noexcept {
    const char* reason = nullptr;
    return prepare_selected_item_dismantle(instanceSoid, true, nullptr, mutation, reason);
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
    return prepare_selected_item_dismantle(instanceSoid, false, &expected, mutation, reason);
}

/** Produces the exact character-and-profile after-image while the captured views remain current. */
bool preview_item_dismantle(const PendingItemDismantle& mutation, AccountState& after) noexcept {
    const AccountState current = account_snapshot();
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
    if (ready) {
        if (!investment::store::write_account(candidate)) {
            investment::store::g_mutex.unlock();
            return false;
        }
    }
    investment::store::g_mutex.unlock();

    if (!ready) {
        return fail("stale_or_invalid");
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
