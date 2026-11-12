#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode405 {

/** Web Service opcode a Postmaster vendor sends to return an item from its recovery bucket. */
inline constexpr std::uint16_t kOpcode = 405;

/** Vendor-mediated transfer; the vendor's rule, not the request, picks the destination. */
struct Request {
    std::int16_t vendorIndex{};
    std::int8_t sourceBucket{};
    std::uint64_t instanceSoid{};
    std::int16_t definitionIndex{};
    std::int32_t quantity{};
};

/**
 * Parses the exact 18-byte vendor transfer descriptor.
 * @param message Parsed Web Service envelope.
 * @param request Receives the vendor, source bucket, instance, definition row and quantity.
 * @return True only for a complete request whose closing pad run is zero.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode405
