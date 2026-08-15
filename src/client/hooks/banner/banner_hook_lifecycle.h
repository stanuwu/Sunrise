#pragma once

namespace sunrise::client::hooks::banner {

/**
 * Attaches the orbit banner component bind.
 * @return True when the bind attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the banner component bind. */
void uninstall() noexcept;

/** @return True while the banner bind is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::banner
