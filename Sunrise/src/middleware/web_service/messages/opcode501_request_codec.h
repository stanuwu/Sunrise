#pragma once

#include <cstdint>

#include "../../../state/account/account_state.h"
#include "../web_service_envelope.h"
#include "opcode501_codec.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** The create-character request: the identity triple plus the authored appearance header. */
struct Request {
    std::uint8_t characterClass{};
    std::uint8_t gender{};
    std::uint8_t race{};
    /**
     * The appearance header in record byte order, ready to memcpy into either character
     * encoder's block. Its values index per-race customization tables, so it is only
     * meaningful beside the `race` it arrived with.
     */
    state::AppearanceHeader appearanceHeader{};
};

/**
 * Parses the create-character request body.
 *
 * The payload is a big-endian bitstream, not a byte-aligned struct: every field is written at
 * whatever bit offset the previous one ended on. Wire order is depth-first over
 * `{ int8 triple[3]; CharHeader header; char name[64]; }`, where `triple` is race, gender and
 * class in that order. Each `triple` entry is a 1-bit presence flag followed by 8 value bits
 * biased by +0x80; the header's fields are 8, 16 or 32 bits biased by +0x80, +0x8000 or not at
 * all; and the 64-byte name begins at bit 261. This parser consumes the first 261 bits and
 * ignores the name, which every observed capture leaves as the placeholder "Player".
 *
 * The header is emitted in the record's own little-endian struct layout rather than as parsed
 * fields, since both character encoders want it as an opaque block. Its last field is the one
 * value that is not copied through: the request carries 0 where the record requires 1.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the parsed creation fields.
 * @return True when the payload holds all 261 bits, every presence flag is set, and race,
 *         gender and class are all in range.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
