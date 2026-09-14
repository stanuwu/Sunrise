#include "nat_relay.h"

#include <cstring>

#include "../encoding/byte_order.h"

namespace sunrise::middleware::bap::nat_relay {
namespace {

constexpr std::size_t kFamilyOffset = 0x12;
constexpr std::size_t kAddressOffset = 0x50;
constexpr std::size_t kPortOffset = 0x54;

void put(std::span<std::byte> output, std::size_t offset, std::span<const std::byte> run) noexcept {
    for (std::size_t index = 0; index < run.size(); ++index) {
        output[offset + index] = run[index];
    }
}

} // namespace

bool encode_initiate(const InitiateRelayConnection& request,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (output.size() < initiate::kBodySize) {
        return false;
    }
    for (std::size_t index = 0; index < initiate::kBodySize; ++index) {
        output[index] = std::byte{};
    }
    encoding::write_u16_be(output.subspan(initiate::kKind).first<encoding::kU16Size>(),
                           request.kind);
    put(output, initiate::kPeerAddress, request.peerAddress);
    encoding::write_u16_be(output.subspan(initiate::kFieldA).first<encoding::kU16Size>(),
                           request.fieldA);
    encoding::write_u16_be(output.subspan(initiate::kFieldB).first<encoding::kU16Size>(),
                           request.fieldB);
    written = initiate::kBodySize;
    return true;
}

bool decode_initiate(std::span<const std::byte> body, InitiateRelayConnection& request) noexcept {
    request = InitiateRelayConnection{};
    // The native decoder requires exactly 0x5C bytes.
    if (body.size() != initiate::kBodySize) {
        return false;
    }
    request.kind = encoding::read_u16_be(body.subspan(initiate::kKind).first<encoding::kU16Size>());
    std::memcpy(request.peerAddress.data(), body.data() + initiate::kPeerAddress, kAddressSize);
    request.fieldA =
        encoding::read_u16_be(body.subspan(initiate::kFieldA).first<encoding::kU16Size>());
    request.fieldB =
        encoding::read_u16_be(body.subspan(initiate::kFieldB).first<encoding::kU16Size>());
    return true;
}

bool encode_request_notification(const RequestRelayConnection& notification,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (output.size() < request_notification::kBodySize) {
        return false;
    }
    for (std::size_t index = 0; index < request_notification::kBodySize; ++index) {
        output[index] = std::byte{};
    }
    namespace field = request_notification;
    // These are wire positions, independent of the native in-memory record. The bytes
    // between them are the credential blocks and the four zeroed runs the reader still consumes,
    // and the tail past the session id is padding that only exists to satisfy the size guard.
    encoding::write_u16_be(output.subspan(field::kAddressLength).first<encoding::kU16Size>(),
                           field::kAddressLengthValue);
    put(output, field::kRemoteAddress, notification.remoteAddress);
    encoding::write_u16_be(output.subspan(field::kPort).first<encoding::kU16Size>(),
                           notification.endpointPort);
    encoding::write_u16_be(output.subspan(field::kEndpointKind).first<encoding::kU16Size>(),
                           field::kEndpointKindIpv4);
    encoding::write_u32_be(output.subspan(field::kEndpointAddress).first<encoding::kU32Size>(),
                           notification.endpointAddress);
    encoding::write_u32_be(output.subspan(field::kSessionId).first<encoding::kU32Size>(),
                           notification.sessionId);
    written = field::kBodySize;
    return true;
}

bool decode_request_notification(std::span<const std::byte> body,
                                 RequestRelayConnection& notification) noexcept {
    notification = RequestRelayConnection{};
    namespace field = request_notification;
    if (body.size() < field::kBodySize) {
        return false;
    }
    if (encoding::read_u16_be(body.subspan(field::kAddressLength).first<encoding::kU16Size>())
        != field::kAddressLengthValue) {
        return false;
    }
    if (encoding::read_u16_be(body.subspan(field::kEndpointKind).first<encoding::kU16Size>())
        != field::kEndpointKindIpv4) {
        return false;
    }
    std::memcpy(
        notification.remoteAddress.data(), body.data() + field::kRemoteAddress, kAddressSize);
    notification.endpointPort =
        encoding::read_u16_be(body.subspan(field::kPort).first<encoding::kU16Size>());
    notification.endpointAddress =
        encoding::read_u32_be(body.subspan(field::kEndpointAddress).first<encoding::kU32Size>());
    notification.sessionId =
        encoding::read_u32_be(body.subspan(field::kSessionId).first<encoding::kU32Size>());
    return true;
}

SecureAddress
make_endpoint_address(std::uint32_t address, std::uint16_t port, std::uint16_t family) noexcept {
    // Little-endian, because these are the client's own in-memory struct fields. It copies
    // the blob word for word rather than serialising it, so the host must lay them out the way the
    // client's `0x56`-byte `memcmp` will read them back.
    SecureAddress blob{};
    blob[kFamilyOffset] = static_cast<std::byte>(family & 0xFFU);
    blob[kFamilyOffset + 1] = static_cast<std::byte>((family >> 8U) & 0xFFU);
    for (std::size_t index = 0; index < encoding::kU32Size; ++index) {
        blob[kAddressOffset + index] = static_cast<std::byte>((address >> (8U * index)) & 0xFFU);
    }
    blob[kPortOffset] = static_cast<std::byte>(port & 0xFFU);
    blob[kPortOffset + 1] = static_cast<std::byte>((port >> 8U) & 0xFFU);
    return blob;
}

} // namespace sunrise::middleware::bap::nat_relay
