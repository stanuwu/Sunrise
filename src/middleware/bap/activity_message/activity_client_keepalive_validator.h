#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::client_keepalive {

/** Client keepalive requests use activity message type 16. */
inline constexpr std::uint32_t kMessageType = 16;
/** The opaque client keepalive body is exactly 1 wire byte. */
inline constexpr std::size_t kEncodedSize = 1;

/**
 * Checks one opaque client keepalive body without reading its byte.
 * @param input Complete type-16 payload with no trailing bytes.
 * @return True when exactly one byte is there.
 */
[[nodiscard]] bool validate_client_keepalive(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::activity_message::client_keepalive
