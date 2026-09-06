#include <Windows.h>

#include <array>

#include "../../../middleware/crypto/random_bytes.h"
#include "../../../middleware/encoding/bit_raw.h"
#include "../../../middleware/encoding/bit_reader.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/peer/connect_messages.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../middleware/gameplay/peer/join_messages.h"
#include "../../../middleware/gameplay/peer/peer_container.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../group/group_host.h"
#include "peer_transport_internal.h"

namespace sunrise::server::gameplay::peer {

namespace {

namespace gp = state::gameplay;
namespace wire = middleware::gameplay::peer;
namespace bits = middleware::encoding::bits;

/** The random connect sequence is folded low byte first. */
constexpr unsigned kByteBits = 8;
/** Sequence the first packet to a peer carries, because the head advances before it is written. */
constexpr std::uint16_t kFirstPacketSequence = 1;

/**
 * Fills the address blob that names this host on the direct path.
 * @param receivingPort Host pool port the request arrived on. Zero names the primary port, as it
 * does on the transport's send path.
 * @param output Receives the direct-path address blob.
 */
void local_address(std::uint16_t receivingPort,
                   std::array<std::byte, wire::kAddressBlobSize>& output) noexcept {
    const gp::Endpoint advertised = endpoint::advertised();
    middleware::gameplay::descriptor::write_direct_net_addr(
        advertised.address, receivingPort != 0 ? receivingPort : advertised.port, output);
}

/** @return A random 32-bit sequence, or zero when Windows refused. */
[[nodiscard]] std::uint32_t random_sequence() noexcept {
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    if (!middleware::crypto::random::fill(bytes)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * kByteBits);
    }
    return value;
}

/**
 * Answers the peer's connect establish with this host's own.
 * It goes on the reliable queue because that is where the peer sends its own.
 * @param to Peer endpoint.
 * @param body Both channel ids.
 */
void answer_establish(const gp::Endpoint& to, const wire::ConnectEstablish& body) noexcept {
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    std::size_t size = 0;
    if (!wire::write_establish(writer, body) || !writer.finish(size)) {
        report(core::log::Level::warn, "ev=gameplay stage=establish result=fail reason=encode");
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link. A channel the peer has retired has no link of its own to answer on.
    gp::PeerLink* peer = find_locked(to);
    const bool queued =
        peer != nullptr
        && wire::enqueue_message(peer->outbound,
                                 static_cast<std::uint8_t>(wire::ConnectId::establish),
                                 wire::kEstablishSize,
                                 {buffer.data(), size},
                                 writer.bit_count());
    if (queued) {
        peer->acknowledgementOwed = true;
        peer->outbound.awaitingAcknowledgement = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    report(core::log::Level::info,
           "ev=gameplay stage=establish result=%s local=0x%08X remote=0x%08X",
           queued ? "queued" : "fail",
           body.channelId,
           body.remoteChannelId);
}

/**
 * Answers one connect request with a connect response.
 * @param from Requesting endpoint.
 * @param request Decoded request body.
 * @param now Monotonic tick count.
 */
void answer_connect(const gp::Endpoint& from,
                    const wire::ConnectRequest& request,
                    std::uint64_t now) noexcept {
    wire::ConnectResponse response{};
    // The peer checks both echoed fields and closes the connection on a wrong sequence.
    response.remoteChannelId = request.channelId;
    response.remoteSequence = request.sequence;
    // The client locates its connecting channel by this address, so it must name the host pool
    // port the request reached rather than always the primary port.
    local_address(from.localPort, response.address);
    DisplacedExternals displaced{};
    std::size_t displacedCount = 0;
    std::array<std::uint64_t, gp::kSessionsPerLink> resetSessions{};
    std::size_t resetSessionCount = 0;
    gp::entity_identity::Source resetSource{};

    AcquireSRWLockExclusive(&g_lock);
    // Keyed by endpoint. The client holds one channel per host peer, so a second link would stamp
    // packets with a channel id the client has already retired.
    gp::PeerLink* peer = find_locked(from);
    // A repeat of the same request is a retransmission and leaves the link alone. A different
    // channel or sequence is a new incarnation the peer built without announcing the teardown.
    const bool rebuilt = peer != nullptr
                         && (peer->remoteConnectionSequence != request.channelId
                             || peer->remoteTransportSequence != request.sequence);
    if (peer == nullptr) {
        peer = allocate_locked();
    }
    const bool fresh = peer != nullptr && (peer->stage == gp::PeerStage::absent || rebuilt);
    if (fresh) {
        resetSource = entity_source(*peer);
        invalidate_entity_identity_locked(resetSource);
        // The sessions outlive the channel. The client rebuilds one channel under every group
        // session it holds and rejoins none of them, so dropping them here strands each one.
        const std::array<std::uint64_t, gp::kSessionsPerLink> held =
            peer->stage == gp::PeerStage::absent ? std::array<std::uint64_t, gp::kSessionsPerLink>{}
                                                 : peer->sessions;
        for (const std::uint64_t sessionId : held) {
            if (sessionId != 0) {
                resetSessions[resetSessionCount++] = sessionId;
            }
        }
        displacedCount = collect_displaced_locked(*peer, displaced);
        *peer = {};
        ++g_peerGeneration;
        if (g_peerGeneration == 0) {
            ++g_peerGeneration;
        }
        peer->peerGeneration = g_peerGeneration;
        peer->channelGeneration = g_peerGeneration;
        peer->sessions = held;
        peer->endpoint = from;
        // The channel id is an incarnation counter: the peer refuses one that does not
        // increase, and reads all ones as unset.
        peer->localConnectionSequence = ++g_channelId;
        // The peer builds its receive window from the announced sequence and expects the first
        // packet one past it. This announces the sequence before the first packet, not the first
        // packet itself.
        peer->localTransportSequence =
            (random_sequence() & ~static_cast<std::uint32_t>(gp::kPacketSequenceModulus - 1))
            | static_cast<std::uint32_t>(kFirstPacketSequence - 1);
    }
    if (peer != nullptr) {
        peer->remoteConnectionSequence = request.channelId;
        peer->remoteTransportSequence = request.sequence;
        // The membership update must name the peer's own address, so its own blob is kept.
        peer->remoteAddress = request.address;
        peer->remoteAddressPresent = true;
        // A retransmission must not move an established link back a stage.
        if (fresh) {
            peer->stage = gp::PeerStage::connecting;
        }
        peer->lastTick = now;
        response.channelId = peer->localConnectionSequence;
        response.sequence = peer->localTransportSequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
    notify_external_outcomes(displaced, displacedCount);
    reset_transports(resetSessions.data(), resetSessionCount);
    reset_entity_source(resetSource);
    if (peer == nullptr) {
        report(core::log::Level::warn, "ev=gameplay stage=connect result=fail reason=capacity");
        return;
    }

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    wire::MessageHeader header{static_cast<std::uint8_t>(wire::ConnectId::response),
                               wire::kResponseSize};
    std::size_t size = 0;
    if (!wire::open_container(writer) || !wire::write_header(writer, header)
        || !wire::write_response(writer, response) || !wire::close_container(writer)
        || !writer.finish(size) || !send_transport(from, {buffer.data(), size})) {
        report(core::log::Level::warn, "ev=gameplay stage=connect result=fail reason=send");
        return;
    }
    // A rebuilt link is invisible otherwise: the peer closes the old one silently.
    report(core::log::Level::info,
           "ev=gameplay stage=connect result=ok peer=%u local=0x%08X remote=0x%08X rebuilt=%u",
           from.port,
           response.channelId,
           request.channelId,
           rebuilt ? 1U : 0U);
    // The peer refuses any first reliable record that is not a connect establish, so this must be
    // enqueued before anything else the join produces.
    wire::ConnectEstablish establish{};
    establish.remoteChannelId = response.remoteChannelId;
    establish.channelId = response.channelId;
    answer_establish(from, establish);
}

/**
 * Binds one group session to the link the peer opened for it.
 * @param from Peer endpoint.
 * @param sessionId Session the join request named.
 * @return True when a link now carries that session.
 */
[[nodiscard]] bool bind_session(const gp::Endpoint& from, std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link, whatever it already carries. A join for a second region arrives on the
    // same channel as the first, and out of band when that channel is still being rebuilt.
    gp::PeerLink* const peer = find_locked(from);
    const char* result = "nolink";
    bool bound = false;
    std::uint32_t channel = 0;
    if (peer != nullptr) {
        channel = peer->localConnectionSequence;
        result = "full";
        for (std::uint64_t& slot : peer->sessions) {
            if (slot == sessionId) {
                result = "held";
                bound = true;
                break;
            }
            if (slot == 0) {
                slot = sessionId;
                result = "bound";
                bound = true;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    report(bound ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=link result=%s session=0x%016llX peer=%u local=0x%08X",
           result,
           static_cast<unsigned long long>(sessionId),
           from.port,
           channel);
    return bound;
}

/**
 * Picks the joining peer's own machine id out of its request's peer table.
 * The row is the one whose address is the NetAddr this link's connect request carried. A single
 * row needs no match.
 * @param from Peer endpoint.
 * @param request Decoded join request.
 * @return The machine id, or zero when no row names this link.
 */
[[nodiscard]] std::uint64_t joining_machine_id(const gp::Endpoint& from,
                                               const wire::JoinRequest& request) noexcept {
    std::array<std::byte, gp::kNetAddrBlobSize> address{};
    bool present = false;
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* peer = find_locked(from);
    if (peer != nullptr && peer->remoteAddressPresent) {
        address = peer->remoteAddress;
        present = true;
    }
    ReleaseSRWLockShared(&g_lock);
    for (std::size_t index = 0; present && index < request.peerCount; ++index) {
        if (request.peers[index].address == address) {
            return request.peers[index].machineId;
        }
    }
    return request.peerCount == 1 ? request.peers[0].machineId : 0;
}

/**
 * Admits or refuses one join request. An admitted join binds the session to the link, then
 * publishes the membership snapshot and the join parameters the peer waits on.
 * @param from Peer endpoint.
 * @param request Decoded join request.
 */
void answer_join(const gp::Endpoint& from, const wire::JoinRequest& request) noexcept {
    const std::uint64_t hostSession = endpoint::identity().onlineSessionId;
    wire::RefuseReason reason = wire::RefuseReason::notFound;
    if (wire::admit(request, hostSession, reason)) {
        // The join is the first thing on this link that names the session. A link already
        // carrying it is a retry.
        const bool bound = bind_session(from, request.sessionId);
        const std::uint64_t machineId = joining_machine_id(from, request);
        const bool published =
            bound && group::publish_membership(from, request.joinId, machineId, request.sessionId);
        // The peer needs both before it finishes: the snapshot names it, and the parameter update
        // releases the latch its own tick waits on.
        const bool parameters = bound && group::publish_join_parameters(request.sessionId);
        // Nothing else names what the peer thinks it is joining.
        report(core::log::Level::info,
               "ev=gameplay stage=join result=admit build=%u..%u exe=%u session=0x%016llX "
               "host=0x%016llX join=0x%016llX machine=0x%016llX peers=%u membership=%s "
               "parameters=%s",
               request.minimumBuild,
               request.maximumBuild,
               static_cast<unsigned>(request.executableType),
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(hostSession),
               static_cast<unsigned long long>(request.joinId),
               static_cast<unsigned long long>(machineId),
               request.peerCount,
               published ? "queued" : "fail",
               parameters ? "queued" : "fail");
        return;
    }
    if (!wire::answerable(request)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=join result=drop reason=protocol value=0x%04X",
               static_cast<unsigned>(request.protocolVersion));
        return;
    }
    report(core::log::Level::warn,
           "ev=gameplay stage=join result=refuse reason=%u build=%u..%u exe=%u session=0x%016llX "
           "host=0x%016llX",
           static_cast<unsigned>(reason),
           request.minimumBuild,
           request.maximumBuild,
           static_cast<unsigned>(request.executableType),
           static_cast<unsigned long long>(request.sessionId),
           static_cast<unsigned long long>(hostSession));
    wire::JoinRefuse refusal{};
    refusal.sessionId = request.sessionId;
    refusal.joinId = request.joinId;
    refusal.reason = reason;

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const wire::MessageHeader header{static_cast<std::uint8_t>(wire::JoinId::refuse),
                                     wire::kJoinRefuseSize};
    std::size_t size = 0;
    if (!wire::open_container(writer) || !wire::write_header(writer, header)
        || !wire::write_join_refuse(writer, refusal) || !wire::close_container(writer)
        || !writer.finish(size) || !send_transport(from, {buffer.data(), size})) {
        report(core::log::Level::warn, "ev=gameplay stage=join result=fail reason=send");
        return;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=join result=refuse reason=%u",
           static_cast<unsigned>(refusal.reason));
}

/**
 * Answers one ping with the pong that echoes it.
 * The pair is mandatory: a peer that pings and is never answered treats the link as unreachable.
 * @param from Peer endpoint.
 * @param reader Reader positioned at the ping body.
 * @return True when the body read, whether or not the reply left the endpoint.
 */
[[nodiscard]] bool answer_ping(const gp::Endpoint& from, bits::Reader& reader) noexcept {
    wire::PingBody ping{};
    if (!wire::read_ping(reader, ping)) {
        return false;
    }
    wire::PongBody pong{};
    pong.sequence = ping.sequence;
    pong.timestamp = ping.timestamp;
    const bool sent = send_out_of_band(
        from,
        static_cast<std::uint8_t>(wire::ConnectId::pong),
        wire::kPongSize,
        [&pong](bits::Writer& writer) noexcept { return wire::write_pong(writer, pong); });
    report(sent ? core::log::Level::debug : core::log::Level::warn,
           "ev=gameplay stage=ping result=%s sequence=%u",
           sent ? "answered" : "fail",
           static_cast<unsigned>(ping.sequence));
    return true;
}

} // namespace

/** Sends one already-encoded out-of-band body in its own container. */
bool send_container(const gp::Endpoint& to,
                    std::uint8_t id,
                    std::uint32_t declaredSize,
                    std::span<const std::byte> body,
                    std::size_t bodyBits) noexcept {
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const wire::MessageHeader header{id, declaredSize};
    if (!wire::open_container(writer) || !wire::write_header(writer, header)) {
        return false;
    }
    bits::Reader reader(body);
    if (!bits::copy(reader, writer, bodyBits)) {
        return false;
    }
    std::size_t size = 0;
    if (!wire::close_container(writer) || !writer.finish(size)) {
        return false;
    }
    return send_transport(to, {buffer.data(), size});
}

/** Consumes one out-of-band message container. */
void consume_container(const gp::Endpoint& from,
                       std::span<const std::byte> payload,
                       std::uint64_t now) noexcept {
    bits::Reader reader(payload);
    if (!wire::read_marker(reader)) {
        return;
    }
    for (;;) {
        wire::MessageHeader header{};
        bool present = false;
        if (!wire::read_header(reader, header, present)) {
            report(core::log::Level::debug, "ev=gameplay stage=oob result=drop reason=header");
            return;
        }
        if (!present) {
            return;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::ping)) {
            if (!answer_ping(from, reader)) {
                return;
            }
            continue;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::packetsDiscarded)) {
            std::uint8_t discarded = 0;
            if (!wire::read_packets_discarded(reader, discarded)) {
                return;
            }
            report(core::log::Level::debug,
                   "ev=gameplay stage=discarded result=read packets=%u",
                   static_cast<unsigned>(discarded));
            continue;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::mayday)) {
            wire::MaydayBody mayday{};
            if (!wire::read_mayday(reader, mayday)) {
                return;
            }
            report(core::log::Level::warn,
                   "ev=gameplay stage=mayday result=read session=0x%016llX code=%d",
                   static_cast<unsigned long long>(mayday.sessionId),
                   static_cast<int>(mayday.code));
            continue;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::request)) {
            wire::ConnectRequest request{};
            if (!wire::read_request(reader, request)) {
                return;
            }
            answer_connect(from, request, now);
            continue;
        }
        if (header.id == static_cast<std::uint8_t>(wire::JoinId::request)) {
            wire::JoinRequest request{};
            if (wire::read_join_request(reader, request)) {
                answer_join(from, request);
            }
            // The member table and tail behind the peer table are not decoded, so no later
            // message in this container can be located.
            return;
        }
        if (header.id == static_cast<std::uint8_t>(wire::ConnectId::closed)) {
            wire::ConnectEnd closed{};
            if (!wire::read_closed(reader, closed)) {
                return;
            }
            // The link goes, the sessions stay. The client rebuilds the channel and rejoins none
            // of them, so releasing their activity host sessions here strands every one.
            drop_endpoint(from);
            report(core::log::Level::info,
                   "ev=gameplay stage=peer result=closed reason=%u",
                   static_cast<unsigned>(closed.reason));
            return;
        }
        if (group::consume(from, header.id, reader, now)) {
            continue;
        }
        // A message this host does not decode ends the chain: its body width is unknown, so
        // every message behind it would be read at the wrong offset.
        report(core::log::Level::debug, "ev=gameplay stage=oob result=stop id=%u", header.id);
        return;
    }
}

} // namespace sunrise::server::gameplay::peer
