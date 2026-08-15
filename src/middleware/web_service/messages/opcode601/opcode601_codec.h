#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode601 {

/** Web Service opcode for a loot pickup the Client cannot do on its own. */
inline constexpr std::uint16_t kOpcode = 601;
/** The whole response is 27 bytes: 6 header, 165 body bits, 2 trailer bits. */
inline constexpr std::size_t kResponseSize = 27;

/**
 * Reports whether this request is the loot pickup.
 * No response field reads the request body, so its width and content are not tested.
 * @param message Parsed Web Service envelope.
 * @return True when the opcode matches.
 */
[[nodiscard]] bool parse_request(const Message& message) noexcept;

/**
 * Encodes the fixed-width response that completes the request.
 * @param message Opcode-601 request whose envelope fields are echoed.
 * @param output Caller-owned response storage.
 * @param written Receives the exact response byte count on success.
 * @return True when the opcode matches and the response fits.
 */
[[nodiscard]] bool
encode_response(const Message& message, std::span<std::byte> output, std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode601
