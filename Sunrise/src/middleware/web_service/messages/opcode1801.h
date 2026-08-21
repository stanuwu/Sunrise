#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode1801 {

/** Web Service opcode the Triumphs screen uses to claim one completed record. */
inline constexpr std::uint16_t kOpcode = 1801;

/** The one logical field carried by the native record claim descriptor. */
struct Request {
    std::uint16_t recordIndex{};
};

/**
 * Parses the exact reflected opcode-1801 record claim descriptor.
 *
 * The record is an optional native field, so the descriptor carries a presence bit before its
 * fifteen-bit row, exactly as the Collections pull does. A request naming no record is not a claim
 * and is refused here. The row is not range-checked against the installed record table: that is the
 * caller's decision, not the codec's.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the named record row.
 * @return True only for the complete canonical three-byte request naming a record.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode1801
