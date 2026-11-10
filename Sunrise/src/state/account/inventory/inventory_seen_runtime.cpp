#include "../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../build_data/runtime.h"
#include "../../investment/store_internal.h"
#include "seen_state.h"

namespace sunrise::state::account::inventory {
namespace store = investment::store;
namespace buckets = build_data::inventory::buckets;

/** Profile bitmap indices follow the installed bucket ranges, not the saved list order. */
bool record_profile_seen(const ProfileNewItems& newItems) noexcept {
    store::Transaction transaction;
    AccountState account;
    if (!transaction.ready() || !store::read_account(account)) {
        return false;
    }
    const auto items = std::span(account.profileItems).first(account.profileItemCount);
    std::array<std::uint16_t, kProfileItemCapacity> rows{};
    if (!resolve_profile_rows(items, rows)) {
        return false;
    }
    store::Statement update(
        "UPDATE profile_items SET seen=? WHERE position=? AND definition_hash=?");
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const bool seen = seen_at(newItems, rows[index]);
        if (seen != item.seen && !update.write(seen, index, item.definitionHash)) {
            return false;
        }
    }
    return transaction.commit();
}

/** Seen state follows the instance even when an acquisition temporarily relocates its row. */
bool record_character_seen(const CharacterNewItems& newItems,
                           std::span<const PresentedItemRow> presentation) noexcept {
    store::Transaction transaction;
    AccountState account;
    if (!transaction.ready() || !store::read_account(account)) {
        return false;
    }
    std::size_t selected = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selected = index;
            break;
        }
    }
    namespace loadout = middleware::datagen::family4::loadout;
    loadout::ResolvedLoadout resolved;
    if (selected == account.characterCount || !loadout::resolve(account, selected, resolved)) {
        return false;
    }
    std::array<bool, buckets::kCharacterSlotCapacity> occupied{};
    store::Statement update("UPDATE items SET seen=? WHERE instance_soid=?");
    for (std::size_t index = 0; index < resolved.itemCount; ++index) {
        const auto& item = resolved.items[index];
        auto row = item.inventoryRow;
        for (const auto& overlay : presentation) {
            if (overlay.instanceSoid == item.instance.instanceSoid) {
                row = overlay.inventoryRow;
                break;
            }
        }
        if (row >= occupied.size() || occupied[row]) {
            return false;
        }
        occupied[row] = true;
        const bool seen = seen_at(newItems, row);
        if (seen != item.seen && !update.write(seen, item.instance.instanceSoid)) {
            return false;
        }
    }
    return transaction.commit();
}

} // namespace sunrise::state::account::inventory
