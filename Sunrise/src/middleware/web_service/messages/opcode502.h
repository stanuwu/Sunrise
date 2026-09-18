#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode502 {

/** Web Service opcode for the delete-character request. */
inline constexpr std::uint16_t kOpcode = 502;

/** The targeted character. */
struct Request {
    std::uint64_t characterSoid{};
};

/**
 * Parses the bare 64-bit targeted-character id.
 * @param message Parsed Web Service envelope.
 * @param request Receives the targeted character id.
 * @return True when the whole id is present.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode502
