#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;

/** Prepares one selected-character unequipped item removal without changing account State. */
bool prepare_item_dismantle(std::uint64_t instanceSoid, PendingItemDismantle& mutation) noexcept {
    mutation = {};
    const AccountState account = bound_account_snapshot();
    if (instanceSoid == 0 || !account::valid(account)) {
        report_dismantle("prepare", "fail", "input", 0, 0, instanceSoid, 0, 0, 0, 0, 0);
        return false;
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= account.characterCount
        || !stage_item_dismantle(account, characterIndex, instanceSoid, mutation)) {
        report_dismantle(
            "prepare", "fail", "ownership_or_resolve", 0, 0, instanceSoid, 0, 0, 0, 0, 0);
        return false;
    }

    report_dismantle("prepare",
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
    return true;
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
