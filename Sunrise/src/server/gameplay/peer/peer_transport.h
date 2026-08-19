#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/encoding/bit_writer.h"
#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::peer {

/**
 * Consumes one decrypted transport payload.
 * The first bit picks the grammar: set is an out-of-band container, clear an established packet.
 * @param from Peer endpoint in host order.
 * @param payload Decrypted payload bytes.
 * @param now Monotonic tick count in milliseconds.
 */
void deliver(const state::gameplay::Endpoint& from,
             std::span<const std::byte> payload,
             std::uint64_t now) noexcept;

/**
 * Queues one reliable message for a peer.
 * Nothing is sent here; the next outgoing packet carries it. Links are keyed by session.
 * @param sessionId Group session the link carries.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares for that id.
 * @param body Encoded body bytes.
 * @param bodyBits Meaningful bits in the body.
 * @return True when the whole message fit the peer's send queue.
 */
[[nodiscard]] bool enqueue_reliable(std::uint64_t sessionId,
                                    std::uint8_t id,
                                    std::uint32_t declaredSize,
                                    std::span<const std::byte> body,
                                    std::size_t bodyBits) noexcept;

/**
 * Reports the NetAddr one peer sent in its own connect request.
 * The membership update must name an address the peer recognises as its own.
 * @param sessionId Group session the link carries.
 * @param output Receives the peer's own address only when it has been captured.
 * @return True when the link is known and its address was captured.
 */
[[nodiscard]] bool
remote_address(std::uint64_t sessionId,
               std::array<std::byte, state::gameplay::kNetAddrBlobSize>& output) noexcept;

/** One out-of-band body is staged here before its container is built. */
inline constexpr std::size_t kOutOfBandBodyCapacity = 128;

/**
 * Sends one already-encoded out-of-band body in its own container.
 * @param to Peer endpoint in host order.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares for that id.
 * @param body Encoded body bytes.
 * @param bodyBits Meaningful bits in the body.
 * @return True when the datagram left the endpoint.
 */
[[nodiscard]] bool send_container(const state::gameplay::Endpoint& to,
                                  std::uint8_t id,
                                  std::uint32_t declaredSize,
                                  std::span<const std::byte> body,
                                  std::size_t bodyBits) noexcept;

/**
 * Encodes one out-of-band body and sends it in its own container.
 * @param to Peer endpoint in host order.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares for that id.
 * @param write Callback writing the body into an open writer.
 * @return True when the datagram left the endpoint.
 */
template <typename Body>
[[nodiscard]] bool send_out_of_band(const state::gameplay::Endpoint& to,
                                    std::uint8_t id,
                                    std::uint32_t declaredSize,
                                    Body write) noexcept {
    std::array<std::byte, kOutOfBandBodyCapacity> body{};
    middleware::encoding::bits::Writer writer(body);
    std::size_t size = 0;
    if (!write(writer) || !writer.finish(size)) {
        return false;
    }
    return send_container(to, id, declaredSize, {body.data(), size}, writer.bit_count());
}

/**
 * Binds one link's view signature.
 * The view body names no group session, and one link can carry both a current and a target
 * region, so the link's endpoint is what identifies it.
 * @param from Peer endpoint the view arrived from.
 * @param signature Signature taken from the peer's own view message.
 */
void bind_view(const state::gameplay::Endpoint& from,
               const state::gameplay::ViewSignature& signature) noexcept;

/** @return True once that session's link holds a bound view and has finished connecting. */
[[nodiscard]] bool view_bound(std::uint64_t sessionId) noexcept;

/**
 * Reports how far the link carrying one group session has got.
 * @param stage Receives the stage, or the absent one when no link carries the session.
 * @return True when a link carries it.
 */
[[nodiscard]] bool link_stage(std::uint64_t sessionId, state::gameplay::PeerStage& stage) noexcept;

/** The connect-exchange sequences that separate one link generation from its successor. */
struct LinkIdentity final {
    std::uint32_t localConnectionSequence{};
    std::uint32_t remoteConnectionSequence{};
};

/**
 * Copies the connect sequences of the link carrying one group session.
 * The client rebuilds its channel under the same session id, so anything holding a reference
 * across that rebuild needs these to tell the two links apart.
 * @param sessionId Group session the link carries.
 * @param output Receives both sequences only when a link carries the session.
 * @return True when a link carries it.
 */
[[nodiscard]] bool link_identity(std::uint64_t sessionId, LinkIdentity& output) noexcept;

/**
 * Sends any owed acknowledgement.
 * Without it the peer keeps retransmitting every reliable message it has sent.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Drops the link carrying one group session.
 * @param sessionId Group session the link carries.
 */
void drop(std::uint64_t sessionId) noexcept;

/**
 * Drops every link at one endpoint.
 * A connect-closed names the endpoint, not one session.
 * @param endpoint Peer endpoint in host order.
 */
void drop_endpoint(const state::gameplay::Endpoint& endpoint) noexcept;

/** Drops every peer. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::peer
