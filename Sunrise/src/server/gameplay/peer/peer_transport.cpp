#include "peer_transport.h"

#include <Windows.h>

#include <array>

#include "../../../middleware/crypto/random_bytes.h"
#include "../../../middleware/encoding/bit_reader.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/peer/connect_messages.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../middleware/gameplay/peer/join_messages.h"
#include "../../../middleware/gameplay/peer/peer_container.h"
#include "../../../middleware/gameplay/peer/reliable_assembly.h"
#include "../association/association_host.h"
#include "../dtls/dtls_host.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../group/group_host.h"

namespace sunrise::server::gameplay::peer {

namespace {

namespace wire = middleware::gameplay::peer;
namespace bits = middleware::encoding::bits;

/**
 * Sends one payload over whichever transport the peer arrived on.
 * Records go first. The engine association answers only when no record association exists.
 * @param to Peer endpoint.
 * @param payload Payload bytes.
 * @return True when one of the two carried it.
 */
[[nodiscard]] bool send_transport(const state::gameplay::Endpoint& to,
                                  std::span<const std::byte> payload) noexcept {
    return dtls::send_payload(to, payload) || association::send_payload(to, payload);
}

/** One out-of-band reply fits well inside a single unfragmented payload. */
constexpr std::size_t kReplyCapacity = 1024;
/** Bit position of the payload marker inside its first byte. */
constexpr unsigned kMarkerShift = 7;
/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** Mask of one byte. */
constexpr std::uint32_t kByteMask = 0xFF;
/** Delay sentinel used until a round trip has been measured. */
constexpr std::uint16_t kDelaySentinel = 1023;
/** Packet sequences are published as ten bits. */
constexpr std::uint16_t kPacketSequenceModulus = state::gameplay::kPacketSequenceModulus;
/** Sequence the first packet to a peer carries, because the head advances before it is written. */
constexpr std::uint16_t kFirstPacketSequence = 1;
/** Smallest head-minus-cursor the peer accepts. This host keeps at most one packet in flight. */
constexpr std::uint8_t kMinimumHeadCursor = 1;
/** One packet cannot report more delivered messages than this. */
constexpr std::size_t kMessageReportCapacity = 8;
/**
 * Milliseconds between two resends of the same queue. The peer discards a packet more than 128
 * sequences ahead of its window, so this host must not send faster than the peer does.
 */
constexpr std::uint64_t kResendInterval = 250;

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<state::gameplay::PeerLink, state::gameplay::kAssociationCapacity> g_peers;
/** Channel ids this host hands out. The peer refuses one that does not increase. */
std::uint32_t g_channelId{0};

/** @return True when both endpoints name the same address and port. */
[[nodiscard]] bool same_endpoint(const state::gameplay::Endpoint& left,
                                 const state::gameplay::Endpoint& right) noexcept {
    return left.address == right.address && left.port == right.port;
}

/** @return Peer for one endpoint, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink*
find_locked(const state::gameplay::Endpoint& from) noexcept {
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent
            && same_endpoint(peer.endpoint, from)) {
            return &peer;
        }
    }
    return nullptr;
}

/** @return True when the link carries one group session. Callers hold the lock. */
[[nodiscard]] bool carries_locked(const state::gameplay::PeerLink& peer,
                                  std::uint64_t sessionId) noexcept {
    for (const std::uint64_t held : peer.sessions) {
        if (held == sessionId) {
            return true;
        }
    }
    return false;
}

/** @return Link carrying one group session, or null. Callers hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* find_session_locked(std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent && carries_locked(peer, sessionId)) {
            return &peer;
        }
    }
    return nullptr;
}

/**
 * Resolves the session a message that does not name one belongs to.
 * A link carrying more than one session leaves it unresolved rather than guessing.
 * @param peer Link the message arrived on.
 * @return The session id, or zero when the link carries none or several.
 */
[[nodiscard]] std::uint64_t sole_session_locked(const state::gameplay::PeerLink& peer) noexcept {
    std::uint64_t only = 0;
    for (const std::uint64_t held : peer.sessions) {
        if (held == 0) {
            continue;
        }
        if (only != 0) {
            return 0;
        }
        only = held;
    }
    return only;
}

/**
 * Resolves the session an out-of-band message at one endpoint belongs to.
 * @param from Peer endpoint.
 * @return The session id, or zero when it cannot be resolved.
 */
[[nodiscard]] std::uint64_t session_for_endpoint(const state::gameplay::Endpoint& from) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* const peer = find_locked(from);
    const std::uint64_t only = peer == nullptr ? 0 : sole_session_locked(*peer);
    ReleaseSRWLockShared(&g_lock);
    return only;
}

/** @return A free peer slot, or null. Callers already hold the lock. */
[[nodiscard]] state::gameplay::PeerLink* allocate_locked() noexcept {
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage == state::gameplay::PeerStage::absent) {
            return &peer;
        }
    }
    return nullptr;
}

/** Fills the address blob that names this host on the direct path. */
void local_address(std::array<std::byte, wire::kAddressBlobSize>& output) noexcept {
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    middleware::gameplay::descriptor::write_net_addr(advertised.address, advertised.port, output);
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
 * @param remoteChannelId Channel id the request carried. The link must still hold it.
 * @param body Both channel ids.
 */
void answer_establish(const state::gameplay::Endpoint& to,
                      std::uint32_t remoteChannelId,
                      const wire::ConnectEstablish& body) noexcept {
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    std::size_t size = 0;
    if (!wire::write_establish(writer, body) || !writer.finish(size)) {
        report(core::log::Level::warn, "ev=gameplay stage=establish result=fail reason=encode");
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link. A channel the peer has retired has no link of its own to answer on.
    state::gameplay::PeerLink* peer = find_locked(to);
    const bool queued =
        peer != nullptr && peer->remoteConnectionSequence == remoteChannelId
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
void answer_connect(const state::gameplay::Endpoint& from,
                    const wire::ConnectRequest& request,
                    std::uint64_t now) noexcept {
    wire::ConnectResponse response{};
    // The peer checks both echoed fields and closes the connection on a wrong sequence.
    response.remoteChannelId = request.channelId;
    response.remoteSequence = request.sequence;
    local_address(response.address);

    AcquireSRWLockExclusive(&g_lock);
    // Keyed by endpoint. The client holds one channel per host peer, so a second link would stamp
    // packets with a channel id the client has already retired.
    state::gameplay::PeerLink* peer = find_locked(from);
    // A repeat of the same request is a retransmission and leaves the link alone. A different
    // channel or sequence is a new incarnation the peer built without announcing the teardown.
    const bool rebuilt = peer != nullptr
                         && (peer->remoteConnectionSequence != request.channelId
                             || peer->remoteTransportSequence != request.sequence);
    if (peer == nullptr) {
        peer = allocate_locked();
    }
    const bool fresh =
        peer != nullptr && (peer->stage == state::gameplay::PeerStage::absent || rebuilt);
    if (fresh) {
        // The sessions outlive the channel. The client rebuilds one channel under every group
        // session it holds and rejoins none of them, so dropping them here strands each one.
        const std::array<std::uint64_t, state::gameplay::kSessionsPerLink> held =
            peer->stage == state::gameplay::PeerStage::absent
                ? std::array<std::uint64_t, state::gameplay::kSessionsPerLink>{}
                : peer->sessions;
        *peer = {};
        peer->sessions = held;
        peer->endpoint = from;
        // The channel id is an incarnation counter: the peer refuses one that does not
        // increase, and reads all ones as unset.
        peer->localConnectionSequence = ++g_channelId;
        // The peer builds its receive window from the announced sequence and expects the first
        // packet one past it. This announces the sequence before the first packet, not the first
        // packet itself.
        peer->localTransportSequence =
            (random_sequence() & ~static_cast<std::uint32_t>(kPacketSequenceModulus - 1))
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
            peer->stage = state::gameplay::PeerStage::connecting;
        }
        peer->lastTick = now;
        response.channelId = peer->localConnectionSequence;
        response.sequence = peer->localTransportSequence;
    }
    ReleaseSRWLockExclusive(&g_lock);
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
    answer_establish(from, request.channelId, establish);
}

/**
 * Binds one group session to the link the peer opened for it.
 * @param from Peer endpoint.
 * @param sessionId Session the join request named.
 * @return True when a link now carries that session.
 */
[[nodiscard]] bool bind_session(const state::gameplay::Endpoint& from,
                                std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    // The endpoint's link, whatever it already carries. A join for a second region arrives on the
    // same channel as the first, and out of band when that channel is still being rebuilt.
    state::gameplay::PeerLink* const peer = find_locked(from);
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
 * Applies the admission rules to one join request and answers it.
 * A protocol mismatch is dropped with no reply.
 * @param from Requesting endpoint.
 * @param request Decoded admission prefix.
 */
void answer_join(const state::gameplay::Endpoint& from, const wire::JoinRequest& request) noexcept {
    const std::uint64_t hostSession = endpoint::identity().onlineSessionId;
    wire::RefuseReason reason = wire::RefuseReason::notFound;
    if (wire::admit(request, hostSession, reason)) {
        // The join is the first thing on this link that names the session. A link already
        // carrying it is a retry.
        const bool bound = bind_session(from, request.sessionId);
        const bool published =
            bound && group::publish_membership(from, request.joinId, request.sessionId);
        // The peer needs both before it finishes: the snapshot names it, and the parameter update
        // releases the latch its own tick waits on.
        const bool parameters = bound && group::publish_join_parameters(request.sessionId);
        // Nothing else names what the peer thinks it is joining.
        report(core::log::Level::info,
               "ev=gameplay stage=join result=admit build=%u..%u exe=%u session=0x%016llX "
               "host=0x%016llX join=0x%016llX membership=%s parameters=%s",
               request.minimumBuild,
               request.maximumBuild,
               static_cast<unsigned>(request.executableType),
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(hostSession),
               static_cast<unsigned long long>(request.joinId),
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
[[nodiscard]] bool answer_ping(const state::gameplay::Endpoint& from,
                               bits::Reader& reader) noexcept {
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

/**
 * Consumes one out-of-band message container.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_container(const state::gameplay::Endpoint& from,
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
                   "ev=gameplay stage=mayday result=read session=0x%016llX code=%u",
                   static_cast<unsigned long long>(mayday.sessionId),
                   static_cast<unsigned>(mayday.code));
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
            // The rest of the request is address and player tables this host does not decode,
            // so no later message in this container can be located.
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
        if (group::consume(from, session_for_endpoint(from), header.id, reader, now)) {
            continue;
        }
        // A message this host does not decode ends the chain: its body width is unknown, so
        // every message behind it would be read at the wrong offset.
        report(core::log::Level::debug, "ev=gameplay stage=oob result=stop id=%u", header.id);
        return;
    }
}

/**
 * Records one received packet sequence in the acknowledgement history.
 * @param peer Peer receiving the packet.
 * @param sequence Sequence the packet published.
 */
void record_sequence(state::gameplay::PeerLink& peer, std::uint16_t sequence) noexcept {
    if (!peer.ringInitialized) {
        peer.ringInitialized = true;
        peer.receiveHead = sequence;
        peer.received = {};
        return;
    }
    // Add the modulus before subtracting. A bare difference is signed and goes negative on a wrap.
    const std::uint16_t advance = static_cast<std::uint16_t>(
        (sequence + state::gameplay::kPacketSequenceModulus - peer.receiveHead)
        % state::gameplay::kPacketSequenceModulus);
    if (advance == 0 || advance >= state::gameplay::kPacketSequenceHalf) {
        // A repeat or an older packet leaves the published history alone.
        return;
    }
    std::array<bool, state::gameplay::kAckHistory> shifted{};
    for (std::size_t index = 0; index < shifted.size(); ++index) {
        // Entry `index` is the packet `index + 1` before the new head, so the old head lands at
        // `advance - 1`. Anything newer than the old head and older than this packet was skipped.
        if (index + 1 < advance) {
            continue;
        }
        if (index + 1 == advance) {
            shifted[index] = true;
            continue;
        }
        const std::size_t source = index - advance;
        shifted[index] = source < peer.received.size() && peer.received[source];
    }
    peer.received = shifted;
    peer.receiveHead = sequence;
}

/**
 * Applies one reassembled reliable message.
 * @param peer Peer that sent it, held under the lock.
 * @param message Reassembled message and its inner header.
 */
void apply_message(state::gameplay::PeerLink& peer,
                   const wire::AssembledMessage& message) noexcept {
    if (message.id == static_cast<std::uint8_t>(wire::ConnectId::establish)
        && peer.stage == state::gameplay::PeerStage::connecting) {
        // The reliable establish is what moves a connected peer past the out-of-band pair.
        peer.stage = state::gameplay::PeerStage::connected;
    }
}

/**
 * Clears the send queue once the peer acknowledges the packet that carried it.
 * @param peer Peer whose acknowledgement arrived, held under the lock.
 * @param ack Acknowledgement state the packet published.
 * @return True when this acknowledgement emptied the queue.
 */
bool apply_acknowledgement(state::gameplay::PeerLink& peer, const wire::AckState& ack) noexcept {
    if (!peer.outbound.awaitingAcknowledgement
        || !wire::acknowledgement_covers(ack, peer.outbound.sentInPacket)) {
        return false;
    }
    // The peer has the packet, so every fragment in it is delivered. The next sequence is kept
    // because message sequences continue across messages.
    for (state::gameplay::OutboundFragment& fragment : peer.outbound.fragments) {
        fragment = {};
    }
    peer.outbound.count = 0;
    peer.outbound.awaitingAcknowledgement = false;
    return true;
}

/**
 * Consumes one established packet.
 * @param from Peer endpoint.
 * @param payload Whole decrypted payload.
 * @param now Monotonic tick count.
 */
void consume_established(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> payload,
                         std::uint64_t now) noexcept {
    wire::EstablishedPacket packet{};
    if (!wire::decode_established(payload, false, packet)) {
        report(core::log::Level::debug, "ev=gameplay stage=packet result=drop reason=grammar");
        return;
    }
    std::array<std::uint8_t, kMessageReportCapacity> delivered{};
    std::size_t deliveredCount = 0;
    unsigned stage = 0;
    bool queueCleared = false;
    std::uint16_t clearedPacket = 0;
    std::uint64_t sessionId = 0;
    // The reliable window never resynchronises, so a stalled queue is only visible as a refused
    // record against the sequence it is still waiting for.
    std::size_t largeDropped = 0;
    std::uint16_t largeNext = 0;
    std::uint16_t largeFirst = 0;
    bool peerFound = false;
    bool guardAccepted = false;
    std::uint8_t expectedGuard = 0;
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* peer = find_locked(from);
    std::array<wire::AssembledMessage, kMessageReportCapacity> bodies{};
    if (peer != nullptr) {
        peerFound = true;
        expectedGuard = wire::connection_sequence_low2(peer->remoteConnectionSequence);
        guardAccepted = packet.connectionSequenceLow2 == expectedGuard;
    }
    if (guardAccepted) {
        sessionId = sole_session_locked(*peer);
        if (packet.ack.outboundHeadPresent) {
            record_sequence(*peer, packet.ack.outboundHead);
        }
        clearedPacket = peer->outbound.sentInPacket;
        queueCleared = apply_acknowledgement(*peer, packet.ack);
        peer->acknowledgementOwed = true;
        peer->lastTick = now;
        largeDropped = wire::accept_records(packet.large, peer->large);
        largeNext = peer->large.nextSequence;
        largeFirst = packet.large.count == 0 ? 0 : packet.large.records[0].sequence;
        wire::accept_records(packet.small, peer->small);
        wire::AssembledMessage message{};
        while (wire::drain_message(peer->large, message)) {
            apply_message(*peer, message);
            if (deliveredCount < delivered.size()) {
                delivered[deliveredCount] = message.id;
                bodies[deliveredCount] = message;
                ++deliveredCount;
            }
        }
        while (wire::drain_message(peer->small, message)) {
            apply_message(*peer, message);
            if (deliveredCount < delivered.size()) {
                delivered[deliveredCount] = message.id;
                bodies[deliveredCount] = message;
                ++deliveredCount;
            }
        }
        stage = static_cast<unsigned>(peer->stage);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!peerFound) {
        return;
    }
    if (!guardAccepted) {
        report(core::log::Level::debug,
               "ev=gameplay stage=packet result=drop reason=channel_low2 got=%u expect=%u",
               static_cast<unsigned>(packet.connectionSequenceLow2),
               static_cast<unsigned>(expectedGuard));
        return;
    }
    for (std::size_t index = 0; index < deliveredCount; ++index) {
        report(core::log::Level::info,
               "ev=gameplay stage=message result=ok id=%u peerstage=%u",
               static_cast<unsigned>(delivered[index]),
               stage);
        // The connect establish belongs to this layer and apply_message already took it, so
        // handing it to the group layer would only report it as undecoded on every connection.
        const wire::AssembledMessage& body = bodies[index];
        if (body.id == static_cast<std::uint8_t>(wire::ConnectId::establish)) {
            continue;
        }
        // Group handling runs outside the lock because answering takes it again.
        bits::Reader reader({body.bytes.data(), state::gameplay::kReassemblyCapacity});
        if (reader.skip(body.bodyBitOffset)
            && !group::consume(from, sessionId, body.id, reader, now)) {
            report(core::log::Level::debug,
                   "ev=gameplay stage=message result=undecoded id=%u",
                   static_cast<unsigned>(body.id));
        }
    }
    if (queueCleared) {
        report(core::log::Level::info,
               "ev=gameplay stage=sendqueue result=cleared packet=%u base=%u entries=%u",
               static_cast<unsigned>(clearedPacket),
               static_cast<unsigned>(packet.ack.receiveHead),
               static_cast<unsigned>(packet.ack.reportedCount));
    }
    report(core::log::Level::debug,
           "ev=gameplay stage=packet result=ok seq=%u base=%u entries=%u large=%u small=%u "
           "first=%u next=%u drop=%zu",
           static_cast<unsigned>(packet.ack.outboundHead),
           static_cast<unsigned>(packet.ack.receiveHead),
           static_cast<unsigned>(packet.ack.reportedCount),
           static_cast<unsigned>(packet.large.count),
           static_cast<unsigned>(packet.small.count),
           static_cast<unsigned>(largeFirst),
           static_cast<unsigned>(largeNext),
           largeDropped);
}

/**
 * Builds and sends one acknowledgement-only packet.
 * @param peer Peer state copied under the lock before the send.
 * @return True when the packet left the endpoint.
 */
[[nodiscard]] bool send_acknowledgement(const state::gameplay::PeerLink& peer) noexcept {
    wire::AckState ack{};
    ack.outboundHead = peer.outboundHead;
    ack.outboundHeadPresent = peer.outboundHeadPresent;
    // The peer subtracts this from the decoded sequence to place its receive window. A zero
    // collapses that window and the peer discards every packet.
    ack.headMinusCursor = kMinimumHeadCursor;
    ack.receiveHead = peer.receiveHead;
    ack.ringInitialized = peer.ringInitialized;
    ack.received = peer.received;
    // No round trip is timed, so the delay field carries its sentinel.
    ack.delay = kDelaySentinel;

    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const std::uint8_t guard = wire::connection_sequence_low2(peer.localConnectionSequence);
    std::size_t size = 0;
    // Only the 32-byte queue carries this host's messages; the 6-byte queue stays empty.
    if (!wire::write_head_and_ack(writer, guard, ack) || !wire::write_queue(writer, peer.outbound)
        || !wire::write_empty_queue(writer) || !wire::write_absent_filler(writer)
        || !writer.finish(size)) {
        return false;
    }
    return send_transport(peer.endpoint, {buffer.data(), size});
}

} // namespace

/** Consumes one decrypted transport payload. */
void deliver(const state::gameplay::Endpoint& from,
             std::span<const std::byte> payload,
             std::uint64_t now) noexcept {
    if (payload.empty()) {
        return;
    }
    if ((std::to_integer<unsigned>(payload[0]) >> kMarkerShift) != 0) {
        consume_container(from, payload, now);
        return;
    }
    consume_established(from, payload, now);
}

/** Sends one already-encoded out-of-band body in its own container. */
bool send_container(const state::gameplay::Endpoint& to,
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
    std::size_t remaining = bodyBits;
    while (remaining != 0) {
        const auto width = static_cast<std::uint8_t>(remaining < kByteBits ? remaining : kByteBits);
        std::uint64_t value = 0;
        if (!reader.read(width, value) || !writer.write(value, width)) {
            return false;
        }
        remaining -= width;
    }
    std::size_t size = 0;
    if (!wire::close_container(writer) || !writer.finish(size)) {
        return false;
    }
    return send_transport(to, {buffer.data(), size});
}

/** Queues one reliable message for a peer. */
bool enqueue_reliable(std::uint64_t sessionId,
                      std::uint8_t id,
                      std::uint32_t declaredSize,
                      std::span<const std::byte> body,
                      std::size_t bodyBits) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool queued =
        peer != nullptr && wire::enqueue_message(peer->outbound, id, declaredSize, body, bodyBits);
    if (queued) {
        // The next service slice carries it, so the acknowledgement path also flushes sends.
        peer->acknowledgementOwed = true;
        // The queue changed, so the packet it was stamped against no longer carries all of it.
        peer->outbound.awaitingAcknowledgement = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return queued;
}

/** Reports the NetAddr one peer sent in its own connect request. */
bool remote_address(std::uint64_t sessionId,
                    std::array<std::byte, state::gameplay::kNetAddrBlobSize>& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr && peer->remoteAddressPresent;
    if (present) {
        output = peer->remoteAddress;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Binds one peer's view signature. */
void bind_view(const state::gameplay::Endpoint& from,
               const state::gameplay::ViewSignature& signature) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    // Keyed by endpoint, not by session: the view body carries no session id, and a link holding
    // both a current and a target region resolves no sole session to key it by.
    state::gameplay::PeerLink* peer = find_locked(from);
    if (peer != nullptr) {
        peer->view = signature;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Reports whether the link carrying one session holds a bound view and is established. */
bool view_bound(std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    // A bound body alone is not readiness. The link also has to be past its connect exchange, or
    // the view belongs to a channel the peer has already rebuilt.
    const bool ready =
        peer != nullptr && peer->view.bound && peer->stage == state::gameplay::PeerStage::connected;
    ReleaseSRWLockShared(&g_lock);
    return ready;
}

/** Reports how far the link carrying one group session has got. */
bool link_stage(std::uint64_t sessionId, state::gameplay::PeerStage& stage) noexcept {
    stage = state::gameplay::PeerStage::absent;
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr;
    if (present) {
        stage = peer->stage;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Copies the connect sequences of the link carrying one group session. */
bool link_identity(std::uint64_t sessionId, LinkIdentity& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const state::gameplay::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr;
    if (present) {
        output.localConnectionSequence = peer->localConnectionSequence;
        output.remoteConnectionSequence = peer->remoteConnectionSequence;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Sends any owed acknowledgement. */
void service(std::uint64_t now) noexcept {
    std::array<state::gameplay::PeerLink, state::gameplay::kAssociationCapacity> owed{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        // An unacknowledged send queue keeps the packet going out until the peer confirms it.
        // Every packet burns one sequence, so the resend is paced.
        const bool resendDue = peer.outbound.count != 0 && now - peer.lastSend >= kResendInterval;
        const bool due = peer.acknowledgementOwed || resendDue;
        if (peer.stage == state::gameplay::PeerStage::absent || !due) {
            continue;
        }
        peer.acknowledgementOwed = false;
        peer.lastSend = now;
        // Only the first send of the current contents is stamped. A resend carries the same
        // fragments, so re-stamping would move the target past what the peer can acknowledge.
        if (peer.outbound.count != 0 && !peer.outbound.awaitingAcknowledgement) {
            peer.outbound.sentInPacket =
                static_cast<std::uint16_t>((peer.outboundHead + 1) % kPacketSequenceModulus);
            peer.outbound.awaitingAcknowledgement = true;
        }
        // The packet sequence advances here so the copy carries the value it will publish.
        peer.outboundHead =
            static_cast<std::uint16_t>((peer.outboundHead + 1) % kPacketSequenceModulus);
        peer.outboundHeadPresent = true;
        peer.lastTick = now;
        owed[count] = peer;
        ++count;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < count; ++index) {
        if (!send_acknowledgement(owed[index])) {
            report(core::log::Level::debug, "ev=gameplay stage=ack result=fail");
        }
    }
}

/** Drops one group session, leaving the link and its other sessions alone. */
void drop(std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    state::gameplay::PeerLink* const peer = find_session_locked(sessionId);
    if (peer != nullptr) {
        // The channel outlives the session. A leave names one region, and the client keeps playing
        // the other over the same channel.
        for (std::uint64_t& slot : peer->sessions) {
            if (slot == sessionId) {
                slot = 0;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops every link at one endpoint, which is what a connect-closed names. */
void drop_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        if (peer.stage != state::gameplay::PeerStage::absent
            && same_endpoint(peer.endpoint, endpoint)) {
            peer = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops every peer. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (state::gameplay::PeerLink& peer : g_peers) {
        peer = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::gameplay::peer
