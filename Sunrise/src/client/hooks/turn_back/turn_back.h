#pragma once

namespace sunrise::client::hooks::turn_back {
/** Installs the controlled-player quarantine predicate filter. */
[[nodiscard]] bool install() noexcept;

/** Reports whether installation is complete and filtering is still accepted. */
[[nodiscard]] bool available() noexcept;

/** Removes the hook, preserving its state when removal cannot complete safely. */
[[nodiscard]] bool uninstall() noexcept;
} // namespace sunrise::client::hooks::turn_back