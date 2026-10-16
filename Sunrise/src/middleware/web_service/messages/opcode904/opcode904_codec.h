#pragma once

#include <cstdint>

#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode904 {

/** Web Service opcode for vendor acquisitions and interaction replies, including rank claims. */
inline constexpr std::uint16_t kOpcode = 904;
/** Logical -1 selects an interaction without a sale row. */
inline constexpr std::int32_t kAbsentSaleIndex = -1;

/** One vendor request: three biased 16-bit selectors followed by a biased 32-bit sale row. */
struct Request {
    /** Index into the vendor table, the same table 901 indexes. */
    std::int16_t vendorIndex{};
    /** UI slot the click landed on. Not a sale row. */
    std::int16_t slotIndex{};
    /** Reply index for rowless interactions; sale-backed semantics are unresolved. */
    std::int16_t third{};
    /**
     * Sale row of the vendor definition, 32-bit biased by 0x80000000.
     * Logical -1 is the absent marker, and only the full width reads it as such.
     */
    std::int32_t saleIndex{};
    /** True when the body carried the sale-row field. */
    bool hasSaleIndex{};
};

/**
 * Decodes three required selectors and an optional sale row, allowing one trailing byte.
 * @param message Parsed Web Service envelope.
 * @param output Receives the request only when the three leading fields decode.
 * @return True when the opcode matches and those fields are present.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode904
