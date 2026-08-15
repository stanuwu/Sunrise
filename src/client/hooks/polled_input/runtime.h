#pragma once

#include <cstdint>

namespace sunrise::client::hooks::polled_input {

/**
 * Attaches the polled key guards. The game reads its action keys by scanning GetKeyState every
 * frame rather than from window messages, so capturing messages alone does not stop it.
 * @return True when both guards attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both polled key guards. */
void uninstall() noexcept;

/** @return True while both polled key guards are attached. */
[[nodiscard]] bool is_installed() noexcept;

/**
 * Applies the polled-input policy for the current interface visibility.
 * @param visible Current Core visibility state.
 */
void apply_visibility(bool visible) noexcept;

/**
 * Reports one key held to game code until it is released.
 * The game reads its actions from this scan, so this drives the action itself rather than copying
 * its effect. Only game callers see it. The interface still reads the real keyboard.
 * @param virtualKey Key to report held, or zero to report none.
 */
void hold_key(std::uint32_t virtualKey) noexcept;

/** Stops reporting any key held on the game's behalf. */
void release_key() noexcept;

} // namespace sunrise::client::hooks::polled_input
