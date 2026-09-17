#pragma once

namespace sunrise::client::hooks::machine_id {
/** Optional configured transport identity; zero leaves native composition unchanged. */
[[nodiscard]] bool install() noexcept;
/** Reapplies the configured ID after the native producer rebuilds its cache. */
void poll() noexcept;
/** Stops polling and restores the native ID only while this override still owns it. */
[[nodiscard]] bool uninstall() noexcept;
} // namespace sunrise::client::hooks::machine_id
