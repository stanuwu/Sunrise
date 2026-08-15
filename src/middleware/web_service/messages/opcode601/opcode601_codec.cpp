/**
 * Opcode 601 is a loot pickup. The Client sends it when it cannot pick the loot up on its own.
 * The reply is 165 bits and no field can be left out, so the bare echo under-runs the decoder.
 * Nothing reads the three tail fields. The request carries no completion handle.
 */

#include "opcode601_codec.h"

#include <algorithm>
#include <array>

#include "../../../encoding/bit_writer.h"
#include "../../../encoding/byte_order.h"
#include "../../status_fields.h"

namespace sunrise::middleware::web_service::messages::opcode601 {
namespace {

/** The three tail integers after the status pair are 32, 64 and 32 bits. */
constexpr std::uint8_t kTailIntegerWidth = 32;
constexpr std::uint8_t kTailLongWidth = 64;
/** Required fields nothing reads still take their width. Zero is the neutral value. */
constexpr std::uint64_t kUnusedValue = 0;

} // namespace

/** Reports whether this request is the remote loot pickup. */
bool parse_request(const Message& message) noexcept {
    return message.opcode == kOpcode;
}

/** Encodes the status pair and its three-field tail in descriptor order. */
bool encode_response(const Message& message,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (!parse_request(message) || output.size() < kResponseSize) {
        return false;
    }

    std::array<std::byte, kResponseSize> staged{};
    encoding::write_u16_be(std::span(staged).first<encoding::kU16Size>(), message.opcode);
    encoding::write_u32_be(std::span(staged).subspan<encoding::kU16Size, encoding::kU32Size>(),
                           message.transactionId);

    // The status value is the Family-4 version the Client waits for. This route pushes no update,
    // so the default value is right. It names a version the Client already has.
    encoding::bits::Writer writer(std::span(staged).subspan(kEnvelopeHeaderSize));
    bool encoded = status::write_fields(writer, ResponseShape::statusPair, StatusResponse{});
    encoded = encoded && writer.write(kUnusedValue, kTailIntegerWidth)
              && writer.write(kUnusedValue, kTailLongWidth)
              && writer.write(kUnusedValue, kTailIntegerWidth)
              && writer.write(0U, kAbsentTrailerWidth);

    std::size_t payloadSize = 0;
    if (!encoded || !writer.finish(payloadSize)) {
        return false;
    }
    const std::size_t total = kEnvelopeHeaderSize + payloadSize;
    if (output.size() < total) {
        return false;
    }
    std::copy_n(staged.begin(), total, output.begin());
    written = total;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode601
