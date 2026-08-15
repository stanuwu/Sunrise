#include "../../../../encoding/bit_reader.h"
#include "../../../../encoding/byte_order.h"
#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

using encoding::bits::Reader;

/** Fixed byte offsets in the encoded field-one web-service request. */
struct FieldOneLayout final {
    /** The big-endian web-service opcode starts the payload. */
    static constexpr std::size_t opcode = 0;
    /** The opaque big-endian transaction follows the opcode and is never retained. */
    static constexpr std::size_t transaction = opcode + encoding::kU16Size;
    /** The MSB-first request schema follows the opaque transaction. */
    static constexpr std::size_t bitstream = transaction + encoding::kU32Size;
};

/** Opcode 1303 names the only field-one request schema decoded here. */
constexpr std::uint16_t kActivityManagerOpcode = 1'303;
/** The root request kind uses 2 bits and bias 1. */
constexpr std::uint8_t kRequestKindWidth = 2;
constexpr std::int8_t kRequestKindBias = 1;
/** The last schema boolean takes 1 bit. */
constexpr std::uint8_t kTrailingFlagWidth = 1;

} // namespace

/** Decodes one field-one payload. Its transaction and wire bytes are not kept. */
bool parse_field_one(std::span<const std::byte> input,
                     ActivityManagerSelection& selection) noexcept {
    if (input.size() < FieldOneLayout::bitstream
        || encoding::read_u16_be(input.subspan<FieldOneLayout::opcode, encoding::kU16Size>())
               != kActivityManagerOpcode) {
        return false;
    }

    const std::span<const std::byte> stream = input.subspan(FieldOneLayout::bitstream);
    Reader reader(stream);
    const std::size_t streamBits = stream.size() * encoding::kBitsPerByte;
    std::uint64_t encodedKind = 0;
    ActivityManagerSelection parsed{};
    if (!reader.read(kRequestKindWidth, encodedKind)) {
        return false;
    }
    parsed.requestKind =
        static_cast<std::int8_t>(static_cast<std::int64_t>(encodedKind) - kRequestKindBias);
    const std::size_t descriptorStart = streamBits - reader.remaining_bits();
    if (!parse(reader, parsed)) {
        return false;
    }
    // The message that echoes the descriptor replays it as-is, so its exact bits are kept next to
    // the scalars. A descriptor too big for that storage still parses. Only the replay falls back
    // to re-encoding the scalars.
    (void)copy_bits(stream,
                    descriptorStart,
                    streamBits - reader.remaining_bits() - descriptorStart,
                    parsed.descriptorBits,
                    parsed.descriptorBitLength);

    std::uint64_t trailingFlag = 0;
    if (!reader.read(kTrailingFlagWidth, trailingFlag)) {
        return false;
    }
    parsed.trailingFlag = trailingFlag != 0;
    selection = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
