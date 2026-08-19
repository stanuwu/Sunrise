#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::sha1 {

/** SHA-1 produces the 20-byte digest stored in a package block record. */
inline constexpr std::size_t kDigestSize = 20;

using Digest = std::array<std::byte, kDigestSize>;

/** Hashes one buffer with Windows CNG. */
[[nodiscard]] bool hash(std::span<const std::byte> input, Digest& output) noexcept;

} // namespace sunrise::middleware::crypto::sha1
