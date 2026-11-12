#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::crypto::random {

/**
 * Fills a buffer with Windows system randomness.
 * Every ephemeral secret Sunrise owns comes from here, never from an authored default.
 * @param output Storage to overwrite completely.
 * @return True when Windows produced every byte.
 */
[[nodiscard]] bool fill(std::span<std::byte> output) noexcept;

/** Draws one 64-bit seed from system randomness. @return False when Windows refused. */
[[nodiscard]] inline bool draw_seed(std::uint64_t& seed) noexcept {
    return fill(std::as_writable_bytes(std::span(&seed, 1)));
}

} // namespace sunrise::middleware::crypto::random
