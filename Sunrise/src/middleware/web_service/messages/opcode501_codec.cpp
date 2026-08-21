#include "opcode501_codec.h"

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "../../encoding/byte_order.h"
#include "../status_fields.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The create-character response carries the new character's 64-bit object id. */
constexpr std::uint8_t kCharacterSoidWidth = 64;

/** Observed native Shadowkeep create-character request size. */
constexpr std::size_t kRequestWireSize = 97;
/** Width of the fixed request prefix before the descriptor groups. */
constexpr std::uint8_t kPrefixWidth = 3;
/** Number of 16-bit descriptor values in the measured request. */
constexpr std::size_t kU16Count = 14;
/** Number of 32-bit descriptor values in the measured request. */
constexpr std::size_t kU32Count = 17;
/** Width of the opaque trailer at the end of the measured request. */
constexpr std::uint8_t kTrailerWidth = 5;

/** Fixed identity bits shared by the measured Human/Awoken/Exo creator requests. */
constexpr std::uint16_t kIdentityFixedBits = 0x0301U;
/** Race occupies identity bits 10-11. */
constexpr std::uint16_t kRaceMask = 0x0C00U;
/** Gender occupies identity bit 1. */
constexpr std::uint16_t kGenderMask = 0x0002U;
/** Low byte of the measured class descriptor. */
constexpr std::uint16_t kClassLowByte = 0x0080U;
/** Fixed 3-bit request prefix. */
constexpr std::uint8_t kObservedPrefix = 6;

constexpr std::uint16_t kSignedU16Bias = 0x8000U;
constexpr std::uint8_t kSignedU8Bias = 0x80U;

static_assert(kPrefixWidth + kU16Count * 16 + kU32Count * 32 + kTrailerWidth
              == kRequestWireSize * 8);

/** Mechanical split of the verified 97-byte request. */
struct Layout {
    std::uint8_t prefix{};
    std::array<std::uint16_t, kU16Count> fields16{};
    std::array<std::uint32_t, kU32Count> fields32{};
    std::uint8_t trailer{};
};

void write_u16_le(std::span<std::byte, 2> output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::byte>(value);
    output[1] = static_cast<std::byte>(value >> 8U);
}

void write_u32_le(std::span<std::byte, 4> output, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint16_t decode_biased_u16(std::uint16_t wire) noexcept {
    return static_cast<std::uint16_t>(wire - kSignedU16Bias);
}

[[nodiscard]] std::uint16_t decode_biased_u8(std::uint8_t wire) noexcept {
    const auto signedValue = static_cast<std::int16_t>(wire) - kSignedU8Bias;
    return static_cast<std::uint16_t>(signedValue);
}

/** Splits the measured request by native descriptor width without assigning field semantics. */
[[nodiscard]] bool parse_layout(std::span<const std::byte> payload, Layout& layout) noexcept {
    layout = {};
    if (payload.size() != kRequestWireSize) {
        return false;
    }

    encoding::bits::Reader reader(payload);
    std::uint64_t value = 0;
    if (!reader.read(kPrefixWidth, value)) {
        return false;
    }
    layout.prefix = static_cast<std::uint8_t>(value);

    for (std::uint16_t& field : layout.fields16) {
        if (!reader.read(16, value)) {
            return false;
        }
        field = static_cast<std::uint16_t>(value);
    }
    for (std::uint32_t& field : layout.fields32) {
        if (!reader.read(32, value)) {
            return false;
        }
        field = static_cast<std::uint32_t>(value);
    }
    if (!reader.read(kTrailerWidth, value)) {
        return false;
    }
    layout.trailer = static_cast<std::uint8_t>(value);
    return reader.remaining_bits() == 0;
}

} // namespace

/**
 * Decodes the verified identity fields and expands the compact creator presentation source to the
 * native-width blocks consumed by character publication.
 */
bool decode_request(const Message& message, DecodedRequest& request) noexcept {
    request = {};
    if (message.opcode != kOpcode) {
        return false;
    }

    Layout layout{};
    if (!parse_layout(message.payload, layout) || layout.prefix != kObservedPrefix) {
        return false;
    }

    const std::uint16_t identity = layout.fields16[0];
    const std::uint16_t variableIdentity = static_cast<std::uint16_t>(kRaceMask | kGenderMask);
    if ((identity & static_cast<std::uint16_t>(~variableIdentity)) != kIdentityFixedBits) {
        return false;
    }

    request.race = static_cast<std::uint8_t>((identity & kRaceMask) >> 10U);
    request.gender = static_cast<std::uint8_t>((identity & kGenderMask) >> 1U);

    const std::uint16_t classField = layout.fields16[1];
    if ((classField & 0x00FFU) != kClassLowByte) {
        return false;
    }

    const std::uint8_t classHigh = static_cast<std::uint8_t>(classField >> 8U);
    if (request.race > 2 || request.gender > 1 || classHigh < 0x80U || classHigh > 0x82U) {
        return false;
    }
    request.characterClass = static_cast<std::uint8_t>(classHigh - 0x80U);
    request.creatorTrailer = layout.trailer;

    // Four compact descriptor values occupy two 16-bit fields. The native presentation record
    // widens them to 16 bits and places the fourth after the ten signed descriptor lanes.
    const std::uint16_t compactA = layout.fields16[2];
    const std::uint16_t compactB = layout.fields16[3];
    std::array<std::uint16_t, 14> header16{};
    header16[0] = static_cast<std::uint8_t>(compactA >> 8U);
    header16[1] = decode_biased_u8(static_cast<std::uint8_t>(compactA));
    header16[2] = decode_biased_u8(static_cast<std::uint8_t>(compactB >> 8U));

    for (std::size_t index = 0; index < 10; ++index) {
        header16[3 + index] = decode_biased_u16(layout.fields16[4 + index]);
    }
    header16[13] = decode_biased_u8(static_cast<std::uint8_t>(compactB));

    for (std::size_t index = 0; index < header16.size(); ++index) {
        write_u16_le(
            std::span<std::byte, 2>(request.presentationHeader.data() + index * 2, 2),
            header16[index]);
    }
    write_u32_le(std::span<std::byte, 4>(request.presentationHeader.data() + 28, 4),
                 layout.fields32[0]);
    write_u32_le(std::span<std::byte, 4>(request.presentationHeader.data() + 32, 4),
                 layout.fields32[1]);

    for (std::size_t index = 0; index < 9; ++index) {
        write_u32_le(std::span<std::byte, 4>(request.creationHeader.data() + index * 4, 4),
                     layout.fields32[2 + index]);
    }
    for (std::size_t index = 0; index < 6; ++index) {
        write_u32_le(std::span<std::byte, 4>(request.creationTail.data() + index * 4, 4),
                     layout.fields32[11 + index]);
    }

    return true;
}

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
