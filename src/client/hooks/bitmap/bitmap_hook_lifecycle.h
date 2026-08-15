#pragma once

namespace sunrise::client::hooks::bitmap {

/**
 * Attaches the bitmap crash guards. These are required fixes, so they carry no settings switch.
 * Each guard reports its own outcome, so a single miss never disables the others.
 * @return True when every guard attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every bitmap crash guard. */
void uninstall() noexcept;

/** @return True while at least one bitmap crash guard is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::bitmap
