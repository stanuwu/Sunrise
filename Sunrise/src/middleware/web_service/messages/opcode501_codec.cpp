#include "opcode501_codec.h"

#include "../../encoding/bit_writer.h"
#include "../status_fields.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The create-character response carries the new character's 64-bit object id. */
constexpr std::uint8_t kCharacterSoidWidth = 64;

} // namespace

/**
 * Writes the echoed header, the caller's status pair, the character id, and the trailer.
 * @param message Parsed request whose envelope fields are echoed.
 * @param status Refusal code, and the Family-4 revision that carries the new character.
 * @param characterSoid The new character's id; zero on a refusal.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes, zero on failure.
 * @return True when the fixed response fits the output buffer.
 */
bool encode_response(const Message& message,
                     const StatusResponse& status,
                     std::uint64_t characterSoid,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEnvelopeHeaderSize) {
        return false;
    }
    encoding::bits::Writer writer = begin_response(message, output);
    // The id carries no presence bit or bias.
    bool encoded = status::write_fields(writer, ResponseShape::statusPair, status)
                   && writer.write(characterSoid, kCharacterSoidWidth)
                   && writer.write(0, kAbsentTrailerWidth);
    std::size_t payloadSize = 0;
    if (!encoded || !writer.finish(payloadSize)) {
        return false;
    }
    written = kEnvelopeHeaderSize + payloadSize;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode501
