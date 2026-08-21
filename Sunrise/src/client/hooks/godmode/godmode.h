#pragma once

namespace sunrise::client::hooks::godmode {

/** Resolves the damage instruction and applies the saved setting. */
[[nodiscard]] bool install() noexcept;

/** Applies or restores the godmode patch. */
[[nodiscard]] bool set_enabled(bool enabled) noexcept;

/** Restores the original instruction and forgets its resolved address. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::godmode
