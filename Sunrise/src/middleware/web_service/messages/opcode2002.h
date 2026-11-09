#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode2002 {

/** Web Service opcode the Store sends to open a held engram. */
inline constexpr std::uint16_t kOpcode = 2002;

/** The one field of the native opening descriptor. */
struct Request {
    std::uint64_t instanceSoid{};
};

/**
 * Parses the exact reflected opcode-2002 descriptor.
 * @param message Parsed Web Service envelope.
 * @param request Receives the held instance to open.
 * @return True only for the complete canonical 9-byte request whose closing bits are zero.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode2002
