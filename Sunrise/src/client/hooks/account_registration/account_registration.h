#pragma once

namespace sunrise::client::hooks::account_registration {
[[nodiscard]] bool install() noexcept;
[[nodiscard]] bool uninstall() noexcept;
/** True only after the native account-of-interest producer registers the local platform identity.
 */
[[nodiscard]] bool own_entry_registered() noexcept;
/** Callback 304 is safe only while its native friends-manager singleton exists. */
[[nodiscard]] bool friends_manager_ready() noexcept;
} // namespace sunrise::client::hooks::account_registration
