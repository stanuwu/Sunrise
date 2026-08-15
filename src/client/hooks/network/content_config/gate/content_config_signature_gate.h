#pragma once

#include <cstddef>

namespace sunrise::client::hooks::network::content_config::gate {

/** Tracks a zero only when this lifecycle wrote it. */
struct Ownership final {
    std::byte* address{};
    char previous{};
    bool owned{};
};

/**
 * Turns off a checked, writable signature gate with a compare-exchange.
 * @param address Gate byte found from the signed-manifest processor.
 * @param ownership Cleared, then receives the value we replaced.
 * @return True when the gate is zero after the call.
 */
[[nodiscard]] bool disable(std::byte* address, Ownership& ownership) noexcept;

/**
 * Writes the zero again when the Client's own content init has put the gate back. Called only
 * from the content tick, which never runs at the same time as install or restore.
 * @param ownership Updated when this call replaces a nonzero value.
 * @return True when the gate is zero after the call.
 */
[[nodiscard]] bool reassert(Ownership& ownership) noexcept;

/**
 * Puts the old byte back, but only if this lifecycle wrote the zero.
 * @param ownership Cleared after the restore, or when the byte is no longer our zero.
 * @return True when no owned zero is left.
 */
[[nodiscard]] bool restore(Ownership& ownership) noexcept;

/** @param ownership Gate ownership state. @return True when the gate is currently zero. */
[[nodiscard]] bool is_disabled(const Ownership& ownership) noexcept;

} // namespace sunrise::client::hooks::network::content_config::gate
