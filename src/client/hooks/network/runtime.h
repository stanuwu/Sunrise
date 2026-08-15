#pragma once

namespace sunrise::client::hooks::network {

/** Installs game-owned HTTP, BAP, and transport hooks. */
[[nodiscard]] bool install_game() noexcept;

/** Installs late-loaded Steam networking state hooks. */
[[nodiscard]] bool install_platform() noexcept;

/** Removes every installed networking hook group. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True when every game-owned networking hook is attached. */
[[nodiscard]] bool is_game_installed() noexcept;

/** @return True while the game-owned group still needs cleanup. */
[[nodiscard]] bool has_game_ownership() noexcept;

/** @return True when both Steam networking state hooks are attached. */
[[nodiscard]] bool is_platform_installed() noexcept;

/** @return True when both networking hook groups are attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::network
