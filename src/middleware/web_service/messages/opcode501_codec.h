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
 * @param message Parsed request whose envelope fields are echoed.
 * @param characterSoid Character object id; must be published in family 3.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the fixed response fits the output buffer.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   std::uint64_t characterSoid,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
