#pragma once

#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode502 {

/** Web Service opcode for deleting one character from the account roster. */
inline constexpr std::uint16_t kOpcode = 502;

/** The character the Client asked to delete. */
struct Request final {
    std::uint64_t characterSoid{};
};

/**
 * Parses the measured delete-character body.
 * The first 64 payload bits are the character SOID, matching opcode 504's identity layout. The
 * measured request carries one trailing zero byte which is not needed to identify the character.
 */
[[nodiscard]] inline bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    constexpr std::uint8_t kCharacterSoidWidth = 64;
    constexpr std::size_t kMinimumPayloadSize = 8;
    if (message.opcode != kOpcode || message.payload.size() < kMinimumPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    return reader.read(kCharacterSoidWidth, request.characterSoid) && request.characterSoid != 0;
}

} // namespace sunrise::middleware::web_service::messages::opcode502
