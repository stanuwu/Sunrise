#pragma once

#include <cstdint>

#include "../web_service_envelope.h"
#include "opcode501_codec.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** The create-character request's identity triple: race, gender and class. */
struct Request {
    std::uint8_t characterClass{};
    std::uint8_t gender{};
    std::uint8_t race{};
};

/**
 * Parses the create-character request's identity triple.
 *
 * The payload is a big-endian bitstream, not a byte-aligned struct. Wire order is depth-first
 * over `{ int8 triple[3]; CharHeader header; char name[64]; }`, where `triple` is race, gender
 * and class in that order; each `triple` entry is a 1-bit presence flag followed by 8 value bits
 * biased by +0x80. This build has no per-character authored appearance to capture, so the header
 * and name that follow are consumed and discarded rather than decoded field by field.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the parsed identity triple.
 * @return True when the payload holds the full identity-and-header span, every presence flag is
 *         set, and race, gender and class are all in range.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
