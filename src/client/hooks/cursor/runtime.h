#pragma once

namespace sunrise::client::hooks::cursor {

/**
 * Attaches the cursor guards. The game recentres and confines the pointer every frame, which
 * makes the pointer unusable over an open interface, so both calls are answered while it is open.
 * @return True when both guards attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both guards and restores the confinement the game last asked for. */
void uninstall() noexcept;

/** @return True while both cursor guards are attached. */
[[nodiscard]] bool is_installed() noexcept;

/**
 * Applies the cursor policy for the current interface visibility.
 * Must run outside every presentation lock because it calls Win32.
 * @param visible Current Core visibility state.
 */
void apply_visibility(bool visible) noexcept;

} // namespace sunrise::client::hooks::cursor
