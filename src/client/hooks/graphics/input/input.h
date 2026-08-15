#pragma once

#include <Windows.h>

namespace sunrise::client::hooks::graphics::input {

/** WM_USER + 13 makes the game run its presentation call right away. */
inline constexpr UINT kRequiredForwardMessage = WM_USER + 13;

/**
 * Subclasses the output window on this thread. Does not assume a class name.
 * @param window Validated swap-chain output window.
 * @return True when the window is subclassed or already holds the same binding.
 */
[[nodiscard]] bool install(HWND window) noexcept;

/** @return True when the original window procedure is restored or the window is gone. */
[[nodiscard]] bool uninstall() noexcept;

/** @param window Candidate window. @return True when it owns the active subclass. */
[[nodiscard]] bool active(HWND window) noexcept;

/**
 * Subclasses the game's hidden raw-mouse window. It carries every mouse move and button, and it
 * is not the swap-chain output window. The game makes it during startup, so a first try can find
 * nothing and a later frame retries.
 * @return True when the window is subclassed or already holds this binding.
 */
[[nodiscard]] bool install_raw_input_window() noexcept;

/** @return True when the raw-input window procedure is restored or the window is gone. */
[[nodiscard]] bool uninstall_raw_input_window() noexcept;

} // namespace sunrise::client::hooks::graphics::input
