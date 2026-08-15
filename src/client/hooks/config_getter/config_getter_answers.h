#pragma once

namespace sunrise::client::hooks::config_getter {

/** @return Direct internal-linkage config URL getter body. */
[[nodiscard]] void* url_entry_point() noexcept;

/** @return Direct internal-linkage config token getter body. */
[[nodiscard]] void* token_entry_point() noexcept;

} // namespace sunrise::client::hooks::config_getter
