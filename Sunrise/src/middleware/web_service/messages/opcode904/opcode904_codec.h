#pragma once

#include <cstdint>

#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode904 {

/** Web Service opcode for acquiring a quest or other pursuit from a vendor. */
inline constexpr std::uint16_t kOpcode = 904;
/** Logical -1 selects an interaction without a sale row. */
inline constexpr std::int32_t kAbsentSaleIndex = -1;

/** One decoded quest-acquire request: three biased 16-bit fields then one biased 32-bit row. */
struct Request {
    /** Index into the vendor table, the same table 901 indexes. */
    std::int16_t vendorIndex{};
    /** UI slot the click landed on. Not a sale row. */
    std::int16_t slotIndex{};
    /** Supported rowless interactions use this as the reply selector; sale-backed meaning is open.
     */
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
 * Decodes one quest-acquire request body.
 * The four fields account for ten of the eleven body bytes; the trailing byte is skipped.
 * @param message Parsed Web Service envelope.
 * @param output Receives the request only when the three leading fields decode.
 * @return True when the opcode matches and those fields are present.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode904
