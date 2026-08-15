#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode504 {

/** Web Service opcode for the select-character request. */
inline constexpr std::uint16_t kOpcode = 504;

/** The picked character. This is the only place the Client names its choice. */
struct Request {
    std::uint64_t characterSoid{};
};

/**
 * Parses the bare 64-bit picked-character id.
 * The id is taken as sent. Only its width is required.
 * @param message Parsed Web Service envelope.
 * @param request Receives the picked character id.
 * @return True when the whole id is present.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode504
