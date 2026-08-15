#include "protobuf_wire.h"

#include <climits>

namespace sunrise::middleware::protobuf {
namespace {

/** Protobuf field keys reserve 3 low bits for the wire type. */
constexpr unsigned int kWireTypeBits = 3;
/** The 3 wire-type bits form this key mask. */
constexpr std::uint64_t kWireTypeMask = 0x07;
/** Field number 0 is reserved and never valid on the wire. */
constexpr std::uint32_t kInvalidFieldNumber = 0;
/** Protobuf keys carry at most 29 field-number bits. */
constexpr std::uint64_t kMaximumFieldNumber = 0x1FFFFFFF;
/** Low 7 bits of a varint byte carry value data. */
constexpr std::uint8_t kVarintPayloadMask = 0x7F;
/** Each varint byte advances the decoded value by 7 bits. */
constexpr unsigned int kVarintPayloadBits = 7;
/** High varint bit marks another byte as present. */
constexpr std::uint8_t kVarintContinuationMask = 0x80;
/** A 64-bit protobuf varint holds at most 10 bytes. */
constexpr std::size_t kMaximumVarintSize = 10;
/** The 10th uint64 varint byte may carry only bit 63. */
constexpr std::uint8_t kMaximumFinalVarintByte = 0x01;
/** Protobuf wire bytes hold 8 bits. */
constexpr unsigned int kBitsPerByte = 8;
static_assert(CHAR_BIT == kBitsPerByte);

/** @return True when a number can form a protobuf field key. */
[[nodiscard]] constexpr bool valid_field_number(std::uint64_t fieldNumber) noexcept {
    return fieldNumber != kInvalidFieldNumber && fieldNumber <= kMaximumFieldNumber;
}

} // namespace

/** Validates and separates one decoded protobuf field key. */
bool decode_key(std::uint64_t encodedKey, DecodedKey& key) noexcept {
    const std::uint64_t fieldNumber = encodedKey >> kWireTypeBits;
    if (!valid_field_number(fieldNumber)) {
        return false;
    }

    WireType wireType{};
    switch (static_cast<std::uint8_t>(encodedKey & kWireTypeMask)) {
    case static_cast<std::uint8_t>(WireType::varint):
        wireType = WireType::varint;
        break;
    case static_cast<std::uint8_t>(WireType::fixed64):
        wireType = WireType::fixed64;
        break;
    case static_cast<std::uint8_t>(WireType::lengthDelimited):
        wireType = WireType::lengthDelimited;
        break;
    case static_cast<std::uint8_t>(WireType::fixed32):
        wireType = WireType::fixed32;
        break;
    default:
        return false;
    }

    key.fieldNumber = static_cast<std::uint32_t>(fieldNumber);
    key.wireType = wireType;
    return true;
}

/** Forms one protobuf field key after validating its field number. */
bool encode_key(std::uint32_t fieldNumber, WireType wireType, std::uint64_t& encodedKey) noexcept {
    if (!valid_field_number(fieldNumber)) {
        return false;
    }
    encodedKey = (static_cast<std::uint64_t>(fieldNumber) << kWireTypeBits)
                 | static_cast<std::uint8_t>(wireType);
    return true;
}

/** Reads one bounded uint64 protobuf varint using a local cursor. */
bool read_varint(std::span<const std::byte> input,
                 std::size_t& cursor,
                 std::uint64_t& value) noexcept {
    value = 0;
    for (std::size_t index = 0; index < kMaximumVarintSize; ++index) {
        if (cursor == input.size()) {
            return false;
        }
        const auto byte = std::to_integer<std::uint8_t>(input[cursor]);
        // Only one payload bit remains in the 10th byte of a uint64 varint.
        if (index == kMaximumVarintSize - 1 && byte > kMaximumFinalVarintByte) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & kVarintPayloadMask)
                 << (index * kVarintPayloadBits);
        ++cursor;
        if ((byte & kVarintContinuationMask) == 0) {
            return true;
        }
    }
    return false;
}

/** Decodes a little-endian fixed protobuf value from bytes already proven to fit. */
std::uint64_t read_fixed(std::span<const std::byte> input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        value |= std::to_integer<std::uint64_t>(input[index]) << (index * kBitsPerByte);
    }
    return value;
}

/** Measures one uint64 protobuf varint. */
std::size_t varint_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value > kVarintPayloadMask) {
        value >>= kVarintPayloadBits;
        ++size;
    }
    return size;
}

/** Writes one varint into storage already proven big enough. */
std::size_t write_raw_varint(std::uint64_t value, std::span<std::byte> output) noexcept {
    std::size_t written = 0;
    while (value > kVarintPayloadMask) {
        output[written++] =
            static_cast<std::byte>((value & kVarintPayloadMask) | kVarintContinuationMask);
        value >>= kVarintPayloadBits;
    }
    output[written++] = static_cast<std::byte>(value);
    return written;
}

} // namespace sunrise::middleware::protobuf
