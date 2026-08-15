#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/investment/investment.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode503 {

/** Web Service opcode for the account investment bootstrap request. */
inline constexpr std::uint16_t kOpcode = 503;

/** Parsed account bootstrap request fields. */
struct Request {
    std::uint8_t tag{};
    std::uint64_t primarySoid{};
    /** Clear when the request carried no readable nonzero account key. */
    bool hasPrimarySoid{};
};

/**
 * Parses the fixed request prefix while allowing later opaque fields.
 * A short body or a zero key clears `hasPrimarySoid` instead of refusing. The reply must still go
 * out, because an empty one under-runs the Client's decoder.
 * @param message Parsed Web Service envelope.
 * @param request Receives the request tag and account key.
 * @return True when the opcode matches.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

/**
 * Encodes the whole account bootstrap response from domain State.
 * @param message Parsed request whose envelope fields are echoed.
 * @param request Parsed request whose account key is echoed.
 * @param investment Account-wide family-5 and unlock State.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when all State rows are safe and the response fits.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   const Request& request,
                                   const state::InvestmentState& investment,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode503
