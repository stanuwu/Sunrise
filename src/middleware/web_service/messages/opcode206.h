#pragma once

#include "../../queuez/subscription.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode206 {

/** Web Service opcode for one queuez family subscription. */
inline constexpr std::uint16_t kOpcode = 206;

/**
 * Parses the bit-packed family type and root SOID.
 * Both fields are taken as sent. Only their width is required.
 * @param message Parsed Web Service envelope.
 * @param subscription Receives the logical family selector.
 * @return True when both whole fields are present.
 */
[[nodiscard]] bool parse_request(const Message& message,
                                 queuez::Subscription& subscription) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode206
