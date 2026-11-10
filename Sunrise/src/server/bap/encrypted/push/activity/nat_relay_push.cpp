#include "nat_relay_push.h"

#include <algorithm>

#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../gameplay/endpoint/gameplay_endpoint.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}
} // namespace
bool consume_relay_notifications(Session& session,
                                 Scratch& scratch,
                                 std::span<std::byte> response,
                                 std::size_t& written,
                                 bool& touchesScratch) noexcept {
    written = 0;
    nat_relay::service(session);
    const auto endpoint = gameplay::endpoint::relay_endpoint();
    if (!session.authenticated || !endpoint.port) {
        return false;
    }
    auto pending =
        std::find_if(session.relay.pairs.begin(), session.relay.pairs.end(), [](const auto& pair) {
            return pair.notificationPending && pair.pairSession;
        });
    if (pending == session.relay.pairs.end()) {
        return false;
    }
    auto& relay = *pending;
    namespace wire = middleware::bap::nat_relay;
    wire::RequestRelayConnection notification{};
    notification.remoteAddress = relay.remote.address;
    notification.endpointAddress = endpoint.address;
    notification.endpointPort = endpoint.port;
    notification.sessionId = relay.pairSession;
    std::array<std::byte, wire::request_notification::kBodySize> body{};
    std::size_t bodySize = 0, payloadSize = 0, sealedSize = 0, frameSize = 0;
    touchesScratch = true;
    const bool encoded =
        wire::encode_request_notification(notification, body, bodySize)
        && middleware::bap::encode_notification_payload(
            middleware::bap::NotificationService::requestRelayConnection,
            0,
            std::span(body).first(bodySize),
            scratch.sealed,
            payloadSize)
        && middleware::secure_channel::seal_frame(session.sessionKey,
                                                  session.sendNonce,
                                                  std::span(scratch.sealed).first(payloadSize),
                                                  scratch.plaintext,
                                                  sealedSize)
        && middleware::bap::encode_frame(middleware::bap::FrameType::encrypted,
                                         std::span(scratch.plaintext).first(sealedSize),
                                         scratch.framed,
                                         frameSize)
        && frameSize <= response.size();
    if (encoded) {
        std::copy_n(scratch.framed.begin(), frameSize, response.begin());
        written = frameSize;
        middleware::secure_channel::advance_nonce(session.sendNonce);
        relay.notificationPending = false;
    }
    clear_prefix(scratch.sealed, payloadSize);
    clear_prefix(scratch.plaintext, payloadSize + middleware::secure_channel::kFrameTagSize);
    return encoded;
}
} // namespace sunrise::server::bap::encrypted::push::activity
