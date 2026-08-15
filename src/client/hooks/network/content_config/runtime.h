#pragma once

namespace sunrise::client::hooks::network::content_config {

/** Installs the local ContentConfig replacements and native identity observers. */
[[nodiscard]] bool install() noexcept;

/** Stops observing, clears identity State, then removes the observers and the hook group. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True when every hook is in place and the signature gate is off. */
[[nodiscard]] bool is_installed() noexcept;

/** @return True while any hook, gate or live call still needs cleanup. */
[[nodiscard]] bool has_ownership() noexcept;

} // namespace sunrise::client::hooks::network::content_config
