#pragma once

namespace sunrise::client::hooks::assert_handler {

/**
 * Replaces the game's fatal assert handler so an assert reports instead of halting.
 * @return True when the handler is installed or its target was never found.
 */
[[nodiscard]] bool install() noexcept;

/** Restores the game's own handler. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while Sunrise's handler owns the slot. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::assert_handler
