#include "activity_message_notification_encoder.h"

#include <algorithm>

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message {
namespace {

/** Fixed discriminator-1 fields that precede a service-9 activity payload. */
struct NotificationLayout final {
    /** The activity transport discriminator starts the envelope. */
    static constexpr std::size_t discriminator = 0;
    /** The big-endian activity session follows the discriminator. */
    static constexpr std::size_t sessionId = discriminator + sizeof(std::byte);
    /** The big-endian activity message type follows the session. */
    static constexpr std::size_t messageType = sessionId + encoding::kU64Size;
    /** The big-endian payload length follows the message type. */
    static constexpr std::size_t payloadLength = messageType + encoding::kU32Size;
    /** Payload bytes start after every fixed-width service field. */
    static constexpr std::size_t payload = payloadLength + encoding::kU32Size;
};

} // namespace

/** Encodes one discriminator-1 service-9 activity envelope with no BAP status header. */
bool encode_notification(std::uint64_t sessionId,
                         std::uint32_t messageType,
                         std::span<const std::byte> payload,
                         std::span<std::byte> output,
                         std::size_t& written) noexcept {
    written = {};
    if (sessionId == kAbsentSessionId || messageType > kMaximumMessageType
        || payload.size() > kMaximumPayloadSize
        || output.size() < NotificationLayout::payload + payload.size()) {
        return false;
    }

    output[NotificationLayout::discriminator] = kTransportDiscriminator;
    encoding::write_u64_be(output.subspan<NotificationLayout::sessionId, encoding::kU64Size>(),
                           sessionId);
    encoding::write_u32_be(output.subspan<NotificationLayout::messageType, encoding::kU32Size>(),
                           messageType);
    encoding::write_u32_be(output.subspan<NotificationLayout::payloadLength, encoding::kU32Size>(),
                           static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), output.begin() + NotificationLayout::payload);
    written = NotificationLayout::payload + payload.size();
    return true;
}

} // namespace sunrise::middleware::bap::activity_message
