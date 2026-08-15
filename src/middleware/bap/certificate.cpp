#include "certificate.h"

#include <cstdint>

#include "../protobuf/codec.h"

namespace sunrise::middleware::bap::certificate {
namespace {

/** Svc304 and svc305 carry their certificate wrapper in field 3. */
constexpr std::uint32_t kCertificateField = 3;
/** Svc305 field 1 reports success with numeric value zero. */
constexpr std::uint32_t kResultField = 1;
constexpr std::uint64_t kSuccessResult = 0;
/** Protobuf keys reserve three low bits for the wire type. */
constexpr unsigned int kWireTypeBits = 3;
/** The low 7 bits of a varint byte carry value data. */
constexpr std::uint8_t kVarintPayloadMask = 0x7F;
/** Each varint byte advances the encoded value by 7 bits. */
constexpr unsigned int kVarintPayloadBits = 7;
/** High varint bit marks another byte as present. */
constexpr std::uint8_t kVarintContinuationMask = 0x80;

/**
 * Finds the first valid length-delimited protobuf field.
 * @param input Complete protobuf message.
 * @param wantedField Numeric field to borrow.
 * @return Borrowed payload, or an empty span when absent or malformed.
 */
[[nodiscard]] std::span<const std::byte> find_length_delimited(std::span<const std::byte> input,
                                                               std::uint32_t wantedField) noexcept {
    protobuf::Reader reader(input);
    protobuf::Field field{};
    while (reader.next(field)) {
        if (field.fieldNumber == wantedField
            && field.wireType == protobuf::WireType::lengthDelimited) {
            // The first wrapper wins. Later bytes are left unread on purpose.
            return field.bytes;
        }
    }
    return {};
}

/**
 * Measures one unsigned protobuf varint.
 * @param value Unsigned value to encode.
 * @return Encoded byte count.
 */
[[nodiscard]] std::size_t varint_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value > kVarintPayloadMask) {
        value >>= kVarintPayloadBits;
        ++size;
    }
    return size;
}

/**
 * Writes one protobuf varint.
 * @param value Unsigned value to encode.
 * @param output Storage the caller sized with varint_size.
 * @return Encoded byte count.
 */
std::size_t write_varint(std::uint64_t value, std::span<std::byte> output) noexcept {
    std::size_t offset = 0;
    while (value > kVarintPayloadMask) {
        output[offset++] =
            static_cast<std::byte>((value & kVarintPayloadMask) | kVarintContinuationMask);
        value >>= kVarintPayloadBits;
    }
    output[offset++] = static_cast<std::byte>(value);
    return offset;
}

/** @return Protobuf key made from a field number and wire type. */
[[nodiscard]] constexpr std::uint64_t make_key(std::uint32_t field,
                                               protobuf::WireType wire) noexcept {
    return (static_cast<std::uint64_t>(field) << kWireTypeBits) | static_cast<std::uint8_t>(wire);
}

} // namespace

/** Rewraps a svc304 request certificate into the svc305 protobuf body. */
bool encode_response(std::span<const std::byte> requestBody,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    const auto certificate = find_length_delimited(requestBody, kCertificateField);
    const auto resultKey = make_key(kResultField, protobuf::WireType::varint);
    const auto certificateKey = make_key(kCertificateField, protobuf::WireType::lengthDelimited);
    const std::size_t required = varint_size(resultKey) + varint_size(kSuccessResult)
                                 + varint_size(certificateKey) + varint_size(certificate.size())
                                 + certificate.size();
    if (output.size() < required) {
        return false;
    }

    std::size_t offset = 0;
    offset += write_varint(resultKey, output.subspan(offset));
    offset += write_varint(kSuccessResult, output.subspan(offset));
    offset += write_varint(certificateKey, output.subspan(offset));
    offset += write_varint(certificate.size(), output.subspan(offset));
    for (const auto byte : certificate) {
        output[offset++] = byte;
    }
    written = offset;
    return true;
}

} // namespace sunrise::middleware::bap::certificate
