#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/encoding/bit_reader.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../middleware/gameplay/external/composite_entity_codec.h"
#include "../../../middleware/gameplay/external/external_entity_codec.h"
#include "../../../middleware/gameplay/external/simulation_event_codec.h"
#include "../../../middleware/gameplay/group/view_message.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../state/gameplay/definition.h"
#include "../../../state/gameplay/external/entity_identity.h"

namespace sunrise::server::gameplay::peer {

/**
 * Installs the process-lifetime channel-0 codec before gameplay starts.
 * TODO: no caller yet. The session-less channel-0 owner waits on the packet-outcome binding to
 * the established writer.
 */
void install_lane0_codec(
    const middleware::gameplay::external::Lane0Codec& codec,
    void (*outcome)(const void*,
                    std::uint64_t,
                    middleware::gameplay::peer::AckOutcome) noexcept = nullptr) noexcept;

/** Session-aware channel-0 callbacks owned by one gameplay policy runtime. */
struct Lane0Transport final {
    const void* context{};
    middleware::gameplay::external::SimulationEventPayloadCodec payloadCodec{};
    bool (*accepted)(const void*,
                     std::uint64_t,
                     const middleware::gameplay::external::SimulationEventBatch&) noexcept {};
    bool (*write)(const void*,
                  std::uint64_t,
                  std::uint64_t,
                  middleware::encoding::bits::Writer&) noexcept {};
    void (*outcome)(const void*,
                    std::uint64_t,
                    std::uint64_t,
                    middleware::gameplay::peer::AckOutcome) noexcept {};
    void (*reset)(const void*, std::uint64_t) noexcept {};
};

/** Installs one process-lifetime, session-aware channel-0 transport. */
void install_lane0_transport(const Lane0Transport& transport) noexcept;

/** Session-aware channel-2 decode and commit callbacks. */
struct EntityTransport final {
    const void* context{};
    bool (*read)(const void*,
                 const state::gameplay::entity_identity::Source&,
                 middleware::encoding::bits::Reader&,
                 middleware::gameplay::external::EntityBatch&) noexcept {};
    bool (*prepare)(const void*,
                    const state::gameplay::entity_identity::Source&,
                    const middleware::gameplay::external::EntityBatch&,
                    std::uint16_t,
                    bool,
                    std::uint64_t,
                    middleware::gameplay::external::EntityBaselineMutation&) noexcept {};
    bool (*commit)(const void*,
                   const state::gameplay::entity_identity::Source&,
                   const middleware::gameplay::external::EntityBaselineMutation&) noexcept {};
    void (*reset)(const void*, const state::gameplay::entity_identity::Source&) noexcept {};
    /** A derived observer cannot reject an already accepted external packet. */
    void (*observed)(const void*,
                     const state::gameplay::entity_identity::Source&,
                     const middleware::gameplay::external::EntityBatch&,
                     std::uint16_t,
                     bool,
                     std::uint64_t,
                     std::uint64_t) noexcept {};
    std::size_t (*retire)(
        const void*,
        const state::gameplay::entity_identity::Source&,
        std::span<const state::gameplay::entity_identity::RetiredLifetime>) noexcept {};
    void (*advanceEpoch)(const void*,
                         const state::gameplay::entity_identity::Source&,
                         std::uint8_t,
                         std::uint8_t,
                         std::uint64_t) noexcept {};
};

/** Installs one process-lifetime, session-aware channel-2 transport. */
void install_entity_transport(const EntityTransport& transport) noexcept;

/** Retires delivered allocations under the same peer-before-codec lock order as receive. */
[[nodiscard]] std::size_t retire_entity_baselines(
    const state::gameplay::entity_identity::Source& source,
    std::span<const state::gameplay::entity_identity::RetiredLifetime> lifetimes) noexcept;

/** Installs the process-lifetime channel-2 codec and accepted-record sink. */
void install_entity_codec(
    const middleware::gameplay::external::TypePayloadCodec& codec,
    bool (*accepted)(const void*,
                     std::uint64_t,
                     const middleware::gameplay::external::EntityBatch&) noexcept = nullptr,
    const void* acceptedContext = nullptr) noexcept;

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

/** Queues one sessionless reliable message on the exact peer endpoint. */
[[nodiscard]] bool enqueue_reliable(const state::gameplay::Endpoint& endpoint,
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
/** Resolves the native address of the exact endpoint carrying this group. */
[[nodiscard]] bool
remote_address(const state::gameplay::Endpoint& endpoint,
               std::uint64_t sessionId,
               std::array<std::byte, state::gameplay::kNetAddrBlobSize>& output) noexcept;

/** Host-reestablish is the widest out-of-band body currently emitted. */
inline constexpr std::size_t kOutOfBandBodyCapacity = 136;

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

/** Result of retaining one inbound view stage. */
enum class ViewStageResult : std::uint8_t {
    accepted,
    noPeer,
    refused,
};

/** Retains one ordered inbound view stage and returns the exact response owed. */
[[nodiscard]] ViewStageResult
receive_view_stage(const state::gameplay::Endpoint& from,
                   const middleware::gameplay::group::ViewEstablishment& input,
                   middleware::gameplay::group::ViewEstablishment& response,
                   std::uint64_t& generation) noexcept;

/** Commits a view response after its reliable enqueue succeeds. */
[[nodiscard]] bool commit_view_response(const state::gameplay::Endpoint& from,
                                        std::uint64_t generation) noexcept;

/** Opens common reconciliation for one exact ActivityClient generation. */
[[nodiscard]] bool
open_external_common(std::uint64_t groupSessionId,
                     const state::activity::SessionBinding& activity,
                     const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
                     std::uint64_t activityClientGeneration,
                     std::uint8_t replicationEpoch) noexcept;

/** Opens provisional common state on the endpoint before its group join names the session. */
[[nodiscard]] bool
open_external_common(const state::gameplay::Endpoint& endpoint,
                     std::uint64_t groupSessionId,
                     const state::activity::SessionBinding& activity,
                     const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
                     std::uint64_t activityClientGeneration,
                     std::uint8_t replicationEpoch) noexcept;

/** Synchronizes views only after an exact host-authored epoch transition commits. */
[[nodiscard]] std::size_t commit_replication_epoch(const state::activity::SessionBinding& activity,
                                                   std::uint64_t activityClientGeneration,
                                                   std::uint8_t expectedEpoch,
                                                   std::uint8_t nextEpoch) noexcept;

/** Reports whether all native gates permit one outbound external body. */
[[nodiscard]] bool external_outbound_eligible(std::uint64_t groupSessionId) noexcept;

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
    std::uint64_t viewGeneration{};
};

/**
 * Copies the connect sequences of the link carrying one joined or authenticated external group.
 * The client rebuilds its channel under the same session id, so anything holding a reference
 * across that rebuild needs these to tell the two links apart.
 * @param sessionId Group session the link carries.
 * @param output Receives both sequences only when a link carries the session.
 * @return True when a link carries it.
 */
[[nodiscard]] bool link_identity(std::uint64_t sessionId, LinkIdentity& output) noexcept;

/**
 * Sends any owed acknowledgement. Without it the peer keeps retransmitting every reliable
 * message it has sent. A concurrent or reentrant call returns without changing owed work.
 * The active slice owns one reusable peer snapshot; sends still validate the live channel
 * before transmission.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/**
 * Drops the link carrying one group session.
 * @param sessionId Group session the link carries.
 */
void drop(std::uint64_t sessionId) noexcept;
/** Removes one group from its exact endpoint, preserving other members' links. */
void drop(const state::gameplay::Endpoint& endpoint, std::uint64_t sessionId) noexcept;

/**
 * Drops every link at one endpoint.
 * A connect-closed names the endpoint, not one session.
 * @param endpoint Peer endpoint in host order.
 */
void drop_endpoint(const state::gameplay::Endpoint& endpoint) noexcept;

/** Drops every peer. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::peer
