#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode406 {

/** Web Service opcode used by item-state actions such as lock and finisher Favorite. */
inline constexpr std::uint16_t kOpcode = 406;

/** Exact logical fields carried by the native 120-bit item-state descriptor. */
struct Request {
    std::uint64_t instanceSoid{};
    std::uint16_t definitionIndex{};
    /** Accumulated item-state bits, already lifted off the descriptor's bias. */
    std::uint32_t flags{};
};

/**
 * Parses the exact reflected opcode-406 item-state descriptor.
 *
 * Both identities are optional native fields carrying a presence bit, and the state value is
 * biased from INT32_MIN. Only the three supported state bits are accepted.
 *
 * Fields are filled as far as the parse reaches, so a refused request still carries what it
 * decoded for the caller's trace.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the instance, definition row, and unbiased state bits.
 * @return True only for the complete canonical 15-byte request.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode406
