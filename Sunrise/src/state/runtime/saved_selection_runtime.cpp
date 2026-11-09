#include "saved_selection_runtime.h"

#include <mutex>

#include "../investment/store_internal.h"

namespace sunrise::state {
namespace {

/** Reads the whole account, inventory included. */
[[nodiscard]] bool read_view(AccountState& account) noexcept {
    return investment::store::read_account(account);
}

/** Reads only the account and character keys and classes. */
[[nodiscard]] bool read_view(AccountIdentity& account) noexcept {
    return investment::store::read_account_identity(account);
}

/** read_account has already refused an invalid account. */
[[nodiscard]] bool usable(const AccountState& /*account*/) noexcept {
    return true;
}

/** Keys alone cannot be validated further than naming an account. */
[[nodiscard]] bool usable(const AccountIdentity& account) noexcept {
    return account.primarySoid != 0;
}

/** Reads either account view with its selected character's banks under one store lock. */
template <typename Account>
[[nodiscard]] SavedSelectionRead read_selection(SavedSelection<Account>& output) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    if (!read_view(output.account)) {
        return SavedSelectionRead::accountStorage;
    }
    if (!usable(output.account)) {
        return SavedSelectionRead::accountInvalid;
    }
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
    return read_selection(output);
}

/** Only the keys and classes are copied with the banks. */
SavedSelectionRead read_saved_selection(SavedSelection<AccountIdentity>& output) noexcept {
    return read_selection(output);
}

} // namespace sunrise::state
