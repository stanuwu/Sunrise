#pragma once

#include <cstdint>

namespace sunrise::core::settings::steam::platform_identity {

/**
 * Reads the cached platform identity, creating and persisting one on first run.
 * An existing cache that fails its checks is refused rather than replaced, so a corrupted
 * file never silently becomes a different account.
 * @param token Set to the identity on success; left unchanged otherwise.
 * @return False when the cache exists but is unreadable or invalid, or a new identity could
 * not be created or written.
 */
[[nodiscard]] bool load_or_create(std::uint64_t& token) noexcept;

} // namespace sunrise::core::settings::steam::platform_identity
