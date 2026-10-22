#pragma once

namespace sunrise::client::hooks::machine_id {
/** Installs the configured transport identity override; succeeds without changes for a zero ID. */
[[nodiscard]] bool install() noexcept;
/** Reapplies the configured ID after the native producer rebuilds its cache. */
void poll() noexcept;
/** Stops polling and restores the native ID only while this override still owns it. */
[[nodiscard]] bool uninstall() noexcept;
} // namespace sunrise::client::hooks::machine_id
