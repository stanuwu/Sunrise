#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode402 {

/** Web Service opcode used by the Character screen's Dismantle action. */
inline constexpr std::uint16_t kOpcode = 402;
/** A dismantled instance is a single unit; a stack request carries its own quantity. */
inline constexpr std::uint32_t kSingleQuantity = 1;

/** Exact logical fields carried by the native 128-bit dismantle descriptor. */
struct Request {
    /** Zero for a stack, which has no instance. */
    std::uint64_t instanceSoid{};
    std::uint16_t definitionIndex{};
    /** Quantity the Client shows for the row, which is how a stack request names its row. */
    std::int32_t quantity{};
};

/**
 * Parses the exact reflected opcode-402 dismantle descriptor.
 * The required flag and all three pad runs are fixed for this action.
 * Fields are filled as far as the parse reaches, so a refused request still describes itself.
 * @param message Parsed Web Service envelope.
 * @param request Receives the named instance or zero for a stack, its definition row and quantity.
 * @return True only for the complete canonical 16-byte request of an instance or a stack.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode402
