#include "user_message_response.h"

#include <cstdint>

namespace sunrise::middleware::bap::user_message {
namespace {

/** Svc 33 stores its required value in protobuf field 4. */
constexpr std::uint8_t kRequiredFieldNumber = 4;
/** Protobuf field keys reserve three low bits for the wire type. */
constexpr std::uint8_t kWireTypeBitCount = 3;
/** Protobuf wire type zero encodes an unsigned varint. */
constexpr std::uint8_t kVarintWireType = 0;
/** The required field key joins field 4 with the varint wire type. */
constexpr std::byte kRequiredFieldTag{
    static_cast<std::uint8_t>((kRequiredFieldNumber << kWireTypeBitCount) | kVarintWireType)};
/** The smallest seen response encodes required field 4 as zero. */
constexpr std::byte kRequiredFieldValue{0x00};
/** The required field tag starts the minimal response body. */
constexpr std::size_t kFieldTagOffset = 0;
/** The 1-byte zero varint follows its field tag. */
constexpr std::size_t kFieldValueOffset = kFieldTagOffset + 1;
/** No optional string fields follow the required field in the minimal response. */
constexpr std::size_t kResponseSize = kFieldValueOffset + 1;

} // namespace

/** Encodes the minimal svc-33 response with all optional strings omitted. */
bool encode_minimal_response(std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kResponseSize) {
        return false;
    }
    output[kFieldTagOffset] = kRequiredFieldTag;
    output[kFieldValueOffset] = kRequiredFieldValue;
    written = kResponseSize;
    return true;
}

} // namespace sunrise::middleware::bap::user_message
