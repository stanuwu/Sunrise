#include "opcode501_codec.h"

#include "../../encoding/bit_writer.h"
#include "../../encoding/byte_order.h"
#include "../status_fields.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The create-character response carries the new character's 64-bit object id. */
constexpr std::uint8_t kCharacterSoidWidth = 64;

} // namespace

/** Writes the echoed header, the shared status pair, the character id, and the trailer. */
bool encode_response(const Message& message,
                     std::uint64_t characterSoid,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEnvelopeHeaderSize) {
        return false;
    }
    encoding::write_u16_be(output.first<encoding::kU16Size>(), message.opcode);
    encoding::write_u32_be(output.subspan<encoding::kU16Size, encoding::kU32Size>(),
                           message.transactionId);

    encoding::bits::Writer writer(output.subspan(kEnvelopeHeaderSize));
    // The id carries no presence bit or bias.
    bool encoded = status::write_fields(writer, ResponseShape::statusPair, StatusResponse{})
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
