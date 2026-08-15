#pragma once

namespace sunrise::client::hooks::egress {

/** @return True only after the owning module is pinned and every lifetime guard is attached. */
[[nodiscard]] bool install() noexcept;

/** @return True when every required guard detour is attached. */
[[nodiscard]] bool is_installed() noexcept;

/**
 * Reports the guard's attach result per export. The guard installs from DllMain, before Core
 * logging exists, so the result is recorded there and logged here.
 */
void report_installation() noexcept;

} // namespace sunrise::client::hooks::egress
