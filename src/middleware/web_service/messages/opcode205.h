#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/investment/investment.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode205 {

/** Web Service opcode for the family-5 investment snapshot request. */
inline constexpr std::uint16_t kOpcode = 205;

/**
 * Reports whether this request is the family-5 snapshot.
 * No response field reads the request body, so its width is not tested.
 * @param message Parsed Web Service envelope.
 * @return True when the opcode matches.
 */
[[nodiscard]] bool parse_request(const Message& message) noexcept;

/**
 * Encodes the whole family-5 investment response from one State snapshot.
 * @param message Parsed request whose envelope fields are echoed.
 * @param investment Account-wide family-5 and unlock State.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the request and all State rows are safe and the response fits.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   const state::InvestmentState& investment,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode205
