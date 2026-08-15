#pragma once

namespace sunrise::client::hooks::bootflow {

/**
 * Attaches the boot-step fixes that carry sign-in through to character select.
 * Each fix reports its own outcome, so a single miss never disables the others.
 * @return True when every fix attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every boot-step fix. */
void uninstall() noexcept;

/** @return True while at least one boot-step fix is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::bootflow
