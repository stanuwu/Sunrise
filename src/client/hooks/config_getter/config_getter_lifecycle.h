#pragma once

namespace sunrise::client::hooks::config_getter {

/**
 * Answers the Client's config URL and token questions.
 * @return True when both answer, or their targets were never found.
 */
[[nodiscard]] bool install() noexcept;

/** Restores both thunks. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while both thunks answer. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::config_getter
