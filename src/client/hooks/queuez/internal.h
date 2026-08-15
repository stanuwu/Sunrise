#pragma once

namespace sunrise::client::hooks::queuez {

/**
 * Attaches the family-zero source-list seed and the account-key capture it needs.
 * @return True when every target was found and its detour attached.
 */
[[nodiscard]] bool install_family0_subscription() noexcept;

/** Detaches the family-zero source-list seed. */
void uninstall_family0_subscription() noexcept;

/** @return True while either family-zero detour is attached. */
[[nodiscard]] bool family0_subscription_installed() noexcept;

/**
 * Attaches the update-notification null-payload guard.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_null_payload_guard() noexcept;

/** Detaches the null-payload guard. */
void uninstall_null_payload_guard() noexcept;

/** @return True while the null-payload guard is attached. */
[[nodiscard]] bool null_payload_guard_installed() noexcept;

} // namespace sunrise::client::hooks::queuez
