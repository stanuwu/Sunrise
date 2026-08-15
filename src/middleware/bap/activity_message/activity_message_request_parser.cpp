#include "activity_message_request_parser.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message {
namespace {

/** Fixed service-8 fields after the generic 6-byte BAP request header. */
struct RequestLayout final {
    /** The optional-header account handle starts the service body. */
    static constexpr std::size_t accountHandle = 0;
    /** The activity envelope follows the big-endian account handle. */
    static constexpr std::size_t discriminator = accountHandle + encoding::kU64Size;
    /** The big-endian activity message type follows the discriminator. */
    static constexpr std::size_t messageType = discriminator + sizeof(std::byte);
    /** The big-endian payload length follows the activity message type. */
    static constexpr std::size_t payloadLength = messageType + encoding::kU32Size;
    /** The runtime peer-heard mask follows the payload length. */
    static constexpr std::size_t peerHeardMask = payloadLength + encoding::kU32Size;
    /** Payload bytes start after every fixed-width service field. */
    static constexpr std::size_t payload = peerHeardMask + encoding::kU32Size;
};

} // namespace

/** Checks one discriminator-1 service-8 body and borrows its exact declared payload. */
bool parse_request(std::span<const std::byte> input, Request& request) noexcept {
    request = {};
    if (input.size() < RequestLayout::payload
        || input[RequestLayout::discriminator] != kTransportDiscriminator) {
        return false;
    }

    const std::uint32_t declaredLength =
        encoding::read_u32_be(input.subspan<RequestLayout::payloadLength, encoding::kU32Size>());
    const std::size_t availableLength = input.size() - RequestLayout::payload;
    if (declaredLength > kMaximumPayloadSize
        || static_cast<std::size_t>(declaredLength) != availableLength) {
        return false;
    }

    Request parsed{};
    parsed.accountHandle =
        encoding::read_u64_be(input.subspan<RequestLayout::accountHandle, encoding::kU64Size>());
    parsed.messageType =
        encoding::read_u32_be(input.subspan<RequestLayout::messageType, encoding::kU32Size>());
    parsed.peerHeardMask =
        encoding::read_u32_be(input.subspan<RequestLayout::peerHeardMask, encoding::kU32Size>());
    parsed.payload = input.subspan(RequestLayout::payload, availableLength);
    request = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message
