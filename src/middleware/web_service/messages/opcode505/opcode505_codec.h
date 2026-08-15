#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode505 {

/** Web Service opcode for the change-character transition. */
inline constexpr std::uint16_t kOpcode = 505;
/** The whole response envelope is 11 bytes. */
inline constexpr std::size_t kResponseSize = 11;

/**
 * Reports whether this request is the change-character transition.
 * No response field reads the request body, so its width and content are not tested.
 * @param message Parsed Web Service envelope.
 * @return True when the opcode matches.
 */
[[nodiscard]] bool parse_request(const Message& message) noexcept;

/**
 * Encodes the successful response with the family-4 version to wait for.
 * @param message Opcode-505 request whose envelope fields are echoed.
 * @param nextVersion Family-4 version published by the paired update.
 * @param output Caller-owned response storage.
 * @param written Receives the exact response byte count on success.
 * @return True when the opcode matches and the response fits.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   std::int32_t nextVersion,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode505
