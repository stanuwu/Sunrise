#pragma once

#include <cstdint>

#include "../../../state/account/account_state.h"
#include "../web_service_envelope.h"
#include "opcode501_codec.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** The create-character request: the identity triple plus the authored appearance header. */
struct Request {
    std::uint8_t characterClass{};
    std::uint8_t gender{};
    std::uint8_t race{};
    /** Record byte order. Its values index per-race tables, so it only fits its own `race`. */
    state::AppearanceHeader appearanceHeader{};
};

/**
 * Parses the create-character request body, a big-endian bitstream with no byte alignment.
 * Race, gender and class each carry a presence bit and a +0x80 bias; the header follows.
 * @param message Parsed Web Service envelope.
 * @param request Receives the creation fields, the header in record byte order.
 * @return False when the payload is short or a presence flag is clear. The values are not
 *         range-checked here; the account check refuses an out-of-range one.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
