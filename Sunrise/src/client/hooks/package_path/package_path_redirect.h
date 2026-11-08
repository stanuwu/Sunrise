#pragma once

namespace sunrise::client::hooks::package_path {

/**
 * Points package files that mods serve at their mod or merged file instead of `packages\`, and
 * writes a mod's kept live updates into a tag when it loads.
 * Attaches only when a loaded mod serves at least one package file.
 * @return True when attached or not needed; false when a mod needs it and it could not attach.
 */
[[nodiscard]] bool install() noexcept;

/** @return True when none of the three detours is attached. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::package_path
