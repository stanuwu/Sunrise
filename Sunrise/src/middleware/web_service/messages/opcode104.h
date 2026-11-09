#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode104 {

/** Web Service opcode the Store sends to ask for a wallet sync. */
inline constexpr std::uint16_t kOpcode = 104;

/** The two booleans the request carries. Their meanings are not decoded. */
struct Request {
    bool first{};
    bool second{};
};

/**
 * Parses the exact one-byte opcode-104 request.
 * @param message Parsed Web Service envelope.
 * @param output Receives both booleans; cleared on failure.
 * @return True only when both envelope blob flags are absent and the padding is clear.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode104
