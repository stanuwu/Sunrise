#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../state/runtime/state.h"

namespace sunrise::middleware::secure_channel {

/** Fixed byte size of the authenticated service-26 envelope. */
inline constexpr std::size_t kServerHelloEnvelopeSize = 84;

/** Authentication-tag bytes prefixed to each encrypted frame payload. */
inline constexpr std::size_t kFrameTagSize = 16;

/** Encodes the authenticated service-26 envelope from runtime State. */
[[nodiscard]] bool encode_server_hello(const state::SignOnState& signOn,
                                       const state::BapState& bap,
                                       std::span<std::byte> output,
                                       std::size_t& written) noexcept;

/** Seals one frameType-1 plaintext as a tag followed by ciphertext. */
[[nodiscard]] bool seal_frame(std::span<const std::byte, state::kAesKeySize> key,
                              std::span<const std::byte, state::kBapNonceSize> nonce,
                              std::span<const std::byte> plaintext,
                              std::span<std::byte> output,
                              std::size_t& written) noexcept;

/** Opens one frameType-1 tag and ciphertext payload. */
[[nodiscard]] bool open_frame(std::span<const std::byte, state::kAesKeySize> key,
                              std::span<const std::byte, state::kBapNonceSize> nonce,
                              std::span<const std::byte> payload,
                              std::span<std::byte> output,
                              std::size_t& written) noexcept;

/** Advances the 12-byte little-endian frame nonce. */
void advance_nonce(std::span<std::byte, state::kBapNonceSize> nonce) noexcept;

} // namespace sunrise::middleware::secure_channel
