#pragma once

namespace sunrise::client::hooks::account_registration {
/**
 * Attaches to the native account-upsert entry point and resolves the friends-manager singleton
 * `friends_manager_ready` reports on.
 * @return True when already installed, or once both scans resolved and the detour attached.
 */
[[nodiscard]] bool install() noexcept;
/**
 * Detaches the upsert hook once no call is in flight, and clears the tracked registration and
 * friends-manager state.
 * @return True when already uninstalled, or once the detour was removed.
 */
[[nodiscard]] bool uninstall() noexcept;
/** True once the native account-of-interest producer has registered the local identity. */
[[nodiscard]] bool own_entry_registered() noexcept;
/** Callback 304 is safe only while its native friends-manager singleton exists. */
[[nodiscard]] bool friends_manager_ready() noexcept;
} // namespace sunrise::client::hooks::account_registration
