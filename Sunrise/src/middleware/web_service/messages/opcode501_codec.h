#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** Web Service opcode for the create-character request. */
inline constexpr std::uint16_t kOpcode = 501;

/**
 * Encodes the create-character response: the status pair then the character object id.
 * The status is the caller's, because this opcode has no single right answer. A refusal has to
 * say so in the code -- the SOID field alone cannot, since zero is what a refusal would send and
 * the Client reads the code, not the id -- and the value feeds the Family-4 version wait, which
 * needs the revision a staged publication will carry, or kNoFamily4Publication when none will.
 * @param message Parsed request whose envelope fields are echoed.
 * @param status Logical status pair: refusal code, and the revision the wait should expect.
 * @param characterSoid Character object id; must be published in family 3. Zero on a refusal.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the fixed response fits the output buffer.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   const StatusResponse& status,
                                   std::uint64_t characterSoid,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
