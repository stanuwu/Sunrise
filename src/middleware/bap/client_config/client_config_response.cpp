#include "client_config_response.h"

#include <cstdint>

namespace sunrise::middleware::bap::client_config {
namespace {

/** Svc 19 requires protobuf field 1. */
constexpr std::uint8_t kFirstRequiredFieldNumber = 1;
/** Svc 19 also requires protobuf field 2. */
constexpr std::uint8_t kSecondRequiredFieldNumber = 2;
/** Protobuf field keys reserve three low bits for the wire type. */
constexpr std::uint8_t kWireTypeBitCount = 3;
/** Protobuf wire type zero encodes an unsigned varint. */
constexpr std::uint8_t kVarintWireType = 0;
/** The minimal response encodes both required fields as zero. */
constexpr std::byte kRequiredFieldValue{0x00};
/** The first field key starts the minimal response body. */
constexpr std::size_t kFirstFieldTagOffset = 0;
/** The first 1-byte zero varint follows its field key. */
constexpr std::size_t kFirstFieldValueOffset = kFirstFieldTagOffset + 1;
/** The second field key follows the whole first field. */
constexpr std::size_t kSecondFieldTagOffset = kFirstFieldValueOffset + 1;
/** The second 1-byte zero varint follows its field key. */
constexpr std::size_t kSecondFieldValueOffset = kSecondFieldTagOffset + 1;
/** No optional bytes follow the second required field in the minimal response. */
constexpr std::size_t kResponseSize = kSecondFieldValueOffset + 1;

/**
 * Makes one protobuf field key for an unsigned-varint field.
 * @param fieldNumber Positive protobuf field number.
 * @return One-byte field key.
 */
constexpr std::byte field_key(std::uint8_t fieldNumber) noexcept {
    return static_cast<std::byte>((fieldNumber << kWireTypeBitCount) | kVarintWireType);
}

} // namespace

/** Encodes the minimal svc-19 response with its optional bytes omitted. */
bool encode_minimal_response(std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kResponseSize) {
        return false;
    }
    output[kFirstFieldTagOffset] = field_key(kFirstRequiredFieldNumber);
    output[kFirstFieldValueOffset] = kRequiredFieldValue;
    output[kSecondFieldTagOffset] = field_key(kSecondRequiredFieldNumber);
    output[kSecondFieldValueOffset] = kRequiredFieldValue;
    written = kResponseSize;
    return true;
}

} // namespace sunrise::middleware::bap::client_config
