#include <Windows.h>

#include <algorithm>

#include "../../../../middleware/secure_channel/runtime.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::reply {

/**
 * Encodes, seals and frames one correlated status-200 reply.
 * The middle payload and sealed buffers are wiped here on both paths, so only the framed result
 * outlives the call.
 * @param scratch Transform buffers owned by the lock.
 * @param route Service data that names the response service.
 * @param taskId Request id to echo back.
 * @param key Active AES-GCM session key.
 * @param nonce Send-direction nonce for this reply.
 * @param body Encoded response body. Empty when its codec refused.
 * @param framedSize Gets the whole outer-frame size, or zero on failure.
 * @return True when the payload, seal and outer frame all fit.
 */
bool encode(Scratch& scratch,
            const ServiceRoute& route,
            std::uint32_t taskId,
            std::span<const std::byte, state::kAesKeySize> key,
            std::span<const std::byte, state::kBapNonceSize> nonce,
            std::span<const std::byte> body,
            std::size_t& framedSize) noexcept {
    framedSize = 0;
    std::size_t payloadSize = 0;
    std::size_t sealedSize = 0;
    const bool encoded =
        middleware::bap::encode_response_payload(
            route.response, taskId, body, scratch.responsePayload, payloadSize)
        && middleware::secure_channel::seal_frame(
            key,
            nonce,
            std::span(scratch.responsePayload).first(payloadSize),
            scratch.sealed,
            sealedSize)
        && middleware::bap::encode_frame(middleware::bap::FrameType::encrypted,
                                         std::span(scratch.sealed).first(sealedSize),
                                         scratch.framed,
                                         framedSize);
    SecureZeroMemory(scratch.responsePayload.data(),
                     (std::min)(scratch.responsePayload.size(), payloadSize));
    SecureZeroMemory(scratch.sealed.data(), (std::min)(scratch.sealed.size(), sealedSize));
    if (!encoded) {
        framedSize = 0;
    }
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::reply
