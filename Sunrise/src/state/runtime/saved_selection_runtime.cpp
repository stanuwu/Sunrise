#include "saved_selection_runtime.h"

#include <mutex>

#include "../investment/store_internal.h"

namespace sunrise::state {
namespace {

/** Reads the banks of the character either account view selects; the caller holds the lock. */
template <typename Account>
[[nodiscard]] SavedSelectionRead read_selected_banks(SavedSelection<Account>& output) noexcept {
    const auto* selected = account::selected_character(output.account);
    if (selected == nullptr) {
        return SavedSelectionRead::unselected;
    }
    output.slot = static_cast<std::size_t>(selected - output.account.characters.data());
    return investment::store::read_unlocks(output.banks, static_cast<int>(output.slot))
               ? SavedSelectionRead::read
               : SavedSelectionRead::bankStorage;
}

} // namespace

/** The inventory and the banks are copied under the same lock. */
SavedSelectionRead read_saved_selection(SavedSelection<AccountState>& output) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    // read_account refuses an invalid account itself.
    if (!investment::store::read_account(output.account)) {
        return SavedSelectionRead::accountStorage;
    }
    return read_selected_banks(output);
}

/** Only the keys and classes are copied with the banks. */
SavedSelectionRead read_saved_selection(SavedSelection<AccountIdentity>& output) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    if (!investment::store::read_account_identity(output.account)) {
        return SavedSelectionRead::accountStorage;
    }
    // Keys alone cannot be validated further than naming an account.
    if (output.account.primarySoid == 0) {
        return SavedSelectionRead::accountInvalid;
    }
    return read_selected_banks(output);
}

} // namespace sunrise::state
