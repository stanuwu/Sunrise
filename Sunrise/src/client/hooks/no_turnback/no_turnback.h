#pragma once

namespace sunrise::client::hooks::no_turnback {

/** Resolves the turnback result and applies the saved setting. */
[[nodiscard]] bool install() noexcept;

/** Applies or restores the turnback result patch. */
[[nodiscard]] bool set_enabled(bool enabled) noexcept;

/** Restores the original instruction and forgets its resolved address. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::no_turnback
