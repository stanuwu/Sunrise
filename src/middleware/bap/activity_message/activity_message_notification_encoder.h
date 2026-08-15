#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::middleware::bap::activity_message {

/** The checked client notification router accepts message types 0 through 58. */
inline constexpr std::uint32_t kMaximumMessageType = 58;

/**
 * Encodes one discriminator-1 service-9 activity envelope with no BAP status header. The caller
 * supplies the message type and the exact payload bytes.
 * @param sessionId Allocated activity session id. Must not be zero.
 * @param messageType Activity message type, 0 through 58, written big-endian.
 * @param payload Activity payload, capped by the client decoder's size.
 * @param output Caller-owned envelope storage, unchanged on failure.
 * @param written Receives the whole envelope size, or zero on failure.
 * @return True when the id, type, payload length and output size are all valid.
 */
[[nodiscard]] bool encode_notification(std::uint64_t sessionId,
                                       std::uint32_t messageType,
                                       std::span<const std::byte> payload,
                                       std::span<std::byte> output,
                                       std::size_t& written) noexcept;

} // namespace sunrise::middleware::bap::activity_message
