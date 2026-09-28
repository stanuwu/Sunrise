#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>

#include "../../../core/threading/srw_lock.h"
#include "../../../middleware/encoding/bit_reader.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../middleware/gameplay/external/common_state.h"
#include "../../../middleware/gameplay/external/control_state_codec.h"
#include "../../../middleware/gameplay/peer/connect_messages.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../middleware/gameplay/peer/outbound_window.h"
#include "../../../middleware/gameplay/peer/packet_fragments.h"
#include "../../../middleware/gameplay/peer/reliable_assembly.h"
#include "../../bap/runtime.h"
#include "../gameplay_log.h"
#include "../group/group_host.h"
#include "peer_packet_fragments.h"
#include "peer_transport_internal.h"

namespace sunrise::server::gameplay::peer {

namespace {

namespace gp = state::gameplay;
namespace wire = middleware::gameplay::peer;
namespace bits = middleware::encoding::bits;
namespace fragments = wire::packet_fragments;

/** Serializes send staging without waiting on another slice or reentrant producer callback. */
core::threading::SrwLock g_sendLock;
/** One peer at a time is encoded outside g_lock; g_sendLock owns this reusable snapshot. */
gp::PeerLink g_sendSnapshot{};

/** Delay sentinel used until a round trip has been measured. */
constexpr std::uint16_t kDelaySentinel = 1023;
/** Smallest head-minus-cursor the peer accepts. This host keeps at most one packet in flight. */
constexpr std::uint8_t kMinimumHeadCursor = 1;
/** One packet cannot report more delivered messages than this. */
constexpr std::size_t kMessageReportCapacity = 8;
/**
 * Milliseconds between two resends of the same queue. The peer discards a packet more than 128
 * sequences ahead of its window, so this host must not send faster than the peer does.
 */
constexpr std::uint64_t kResendInterval = 250;
/** Reflected root 0x80806AE6 after its lane-presence bit. */
constexpr std::size_t kPlayerSnapshotBits = 1373;

/** Eight unique logical packet failures bound the replay sample. */
constexpr std::size_t kRejectedPacketLimit = 8;
/** A rejected logical packet can span every native transport fragment. */
constexpr std::size_t kRejectedPacketCapacity = fragments::kMaximumPacketBytes;
/** A 256-byte hex chunk leaves room for event fields in the 1024-byte log line. */
constexpr std::size_t kRejectedHexChunk = 256;

struct RejectedPacket final {
    gp::entity_identity::Source source{};
    std::array<std::byte, kRejectedPacketCapacity> bytes{};
    std::size_t size{};
};

SRWLOCK g_rejectedPacketLock{SRWLOCK_INIT};
std::array<RejectedPacket, kRejectedPacketLimit> g_rejectedPackets{};
std::size_t g_rejectedPacketCount{};
wire::EstablishedPacket g_rejectedPacketHeader{};

/**
 * Logs complete payload bytes in numbered chunks without truncating a packet.
 * @param capture
 * Process-local capture number.
 * @param payload Complete decrypted datagram.
 */
void log_rejected_hex(std::size_t capture, std::span<const std::byte> payload) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    for (std::size_t offset = 0; offset < payload.size(); offset += kRejectedHexChunk) {
        const auto count = (std::min)(kRejectedHexChunk, payload.size() - offset);
        const int prefix = std::snprintf(
            line.data(),
            line.size(),
            "ev=gameplay stage=rejected_packet_hex capture=%zu offset=%zu bytes=%zu hex=",
            capture,
            offset,
            count);
        if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
            return;
        }
        auto length = static_cast<std::size_t>(prefix);
        if (!core::log::append_hex(line, length, payload.subspan(offset, count))) {
            return;
        }
        core::log::write(
            core::log::Channel::server, core::log::Level::debug, {line.data(), length});
    }
}

/**
 * Retains a bounded replay sample only while server debug logging is enabled.
 * @param source Source already admitted for this decode.
 * @param payload Complete decrypted datagram.
 * @param externalOffset Start of the external handler in bits.
 * @param laneOffset Start of the rejected entity lane in bits.
 * @param stoppedOffset Reader position at failure in bits.
 */
void log_rejected_entity_packet(const gp::entity_identity::Source& source,
                                std::span<const std::byte> payload,
                                std::size_t externalOffset,
                                std::size_t laneOffset,
                                std::size_t stoppedOffset) noexcept {
    if (!core::log::accepts(core::log::Channel::server, core::log::Level::debug) || payload.empty()
        || payload.size() > kRejectedPacketCapacity) {
        return;
    }
    AcquireSRWLockExclusive(&g_rejectedPacketLock);
    bool duplicate = false;
    for (std::size_t index = 0; index < g_rejectedPacketCount; ++index) {
        const auto& prior = g_rejectedPackets[index];
        if (prior.source == source && prior.size == payload.size()
            && std::equal(payload.begin(), payload.end(), prior.bytes.begin())) {
            duplicate = true;
            break;
        }
    }
    if (duplicate || g_rejectedPacketCount == kRejectedPacketLimit) {
        ReleaseSRWLockExclusive(&g_rejectedPacketLock);
        return;
    }
    auto& saved = g_rejectedPackets[g_rejectedPacketCount];
    saved.source = source;
    saved.size = payload.size();
    std::copy(payload.begin(), payload.end(), saved.bytes.begin());
    const auto capture = ++g_rejectedPacketCount;
    const bool headerValid = wire::decode_established(payload, true, g_rejectedPacketHeader);
    const auto sequence = g_rejectedPacketHeader.ack.outboundHead;
    const auto hasSequence = g_rejectedPacketHeader.ack.outboundHeadPresent;
    const auto guard = g_rejectedPacketHeader.connectionSequenceLow2;
    ReleaseSRWLockExclusive(&g_rejectedPacketLock);
    report(core::log::Level::debug,
           "ev=gameplay stage=rejected_packet capture=%zu reason=lane2 bytes=%zu "
           "external_bit=%zu lane_bit=%zu stopped_bit=%zu header=%u sequence=%u has_sequence=%u "
           "guard=%u",
           capture,
           payload.size(),
           externalOffset,
           laneOffset,
           stoppedOffset,
           headerValid ? 1U : 0U,
           static_cast<unsigned>(sequence),
           hasSequence ? 1U : 0U,
           static_cast<unsigned>(guard));
    report(core::log::Level::debug,
           "ev=gameplay stage=rejected_packet_source capture=%zu activity=0x%llX revision=%llu "
           "client_generation=%llu group=0x%llX peer=%llu channel=%llu view=%llu "
           "address=0x%08X port=%u local_port=%u local_sequence=%u remote_sequence=%u",
           capture,
           static_cast<unsigned long long>(source.activitySessionId),
           static_cast<unsigned long long>(source.activityRevision),
           static_cast<unsigned long long>(source.activityClientGeneration),
           static_cast<unsigned long long>(source.groupSessionId),
           static_cast<unsigned long long>(source.peerGeneration),
           static_cast<unsigned long long>(source.channelGeneration),
           static_cast<unsigned long long>(source.viewGeneration),
           source.address,
           static_cast<unsigned>(source.port),
           static_cast<unsigned>(source.localPort),
           source.localConnectionSequence,
           source.remoteConnectionSequence);
    log_rejected_hex(capture, payload);
}

/** Packet ordinals share the receive ring's half-range ordering. */
std::uint64_t packet_ordinal(const gp::PeerLink& peer, std::uint16_t sequence) noexcept {
    if (!peer.ringInitialized) {
        return gp::kPacketSequenceModulus + sequence;
    }
    const auto forward =
        (sequence + gp::kPacketSequenceModulus - peer.receiveHead) % gp::kPacketSequenceModulus;
    return forward < gp::kPacketSequenceHalf
               ? peer.receiveOrdinal + forward
               : peer.receiveOrdinal - (gp::kPacketSequenceModulus - forward);
}

/**
 * Records one received packet sequence in the acknowledgement history.
 * @param peer Peer receiving the packet.
 * @param sequence Sequence the packet published.
 */
void record_sequence(gp::PeerLink& peer, std::uint16_t sequence) noexcept {
    if (!peer.ringInitialized) {
        peer.receiveOrdinal = packet_ordinal(peer, sequence);
        peer.ringInitialized = true;
        peer.receiveHead = sequence;
        peer.received = {};
        return;
    }
    // Add the modulus before subtracting. A bare difference is signed and goes negative on a wrap.
    const std::uint16_t advance = static_cast<std::uint16_t>(
        (sequence + gp::kPacketSequenceModulus - peer.receiveHead) % gp::kPacketSequenceModulus);
    if (advance == 0 || advance >= gp::kPacketSequenceHalf) {
        // A repeat or an older packet leaves the published history alone.
        return;
    }
    std::array<bool, gp::kAckHistory> shifted{};
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
    peer.receiveOrdinal += advance;
    peer.receiveHead = sequence;
}

/**
 * Applies one reassembled reliable message.
 * @param peer Peer that sent it, held under the lock.
 * @param message Reassembled message and its inner header.
 */
void apply_message(gp::PeerLink& peer, const wire::AssembledMessage& message) noexcept {
    if (message.id == static_cast<std::uint8_t>(wire::ConnectId::establish)
        && peer.stage == gp::PeerStage::connecting) {
        // The reliable establish is what moves a connected peer past the out-of-band pair.
        peer.stage = gp::PeerStage::connected;
    }
}

/**
 * Removes the carried fragment run once the peer acknowledges its packet.
 * @param peer Peer whose
 * acknowledgement arrived, held under the lock.
 * @param ack Acknowledgement state the packet published.
 * @return True when this acknowledgement released the outstanding run.
 */
bool apply_acknowledgement(gp::PeerLink& peer, const wire::AckState& ack) noexcept {
    return wire::outbound_window::acknowledge(peer.outbound, ack);
}

/** Common state retained from one complete external frame. */
struct ParsedExternal {
    middleware::gameplay::external::CommonState common{};
    middleware::gameplay::external::SimulationEventBatch lane0{};
    middleware::gameplay::external::ControlStateBatch lane1{};
    middleware::gameplay::external::EntityBatch entities{};
    bool commonPresent{};
};

/** Exact external component that refused a frame. */
enum class ExternalReadResult : std::uint8_t {
    accepted,
    prefix,
    common,
    lane0,
    lane1,
    lane2,
    lane3,
    filler,
};

using middleware::gameplay::external::read_flag;

/** Reads or skips the receive-only player lane without retaining its local snapshot. */
[[nodiscard]] bool read_player_lane(bits::Reader& reader) noexcept {
    bool present = false;
    bool trailingList = false;
    return read_flag(reader, present)
           && (!present || (reader.skip(kPlayerSnapshotBits) && read_flag(reader, trailingList)));
}

/** Reads common, channels 0 to 3, and filler. */
[[nodiscard]] ExternalReadResult
read_external(std::span<const std::byte> payload,
              std::size_t bitOffset,
              const gp::entity_identity::Source& source,
              const middleware::gameplay::external::Lane0Codec& lane0,
              const Lane0Transport& lane0Transport,
              const middleware::gameplay::external::TypePayloadCodec& entities,
              ParsedExternal& output) noexcept {
    output.commonPresent = false;
    bits::Reader reader(payload);
    thread_local std::unique_ptr<ParsedExternal> candidateStorage;
    if (!candidateStorage) {
        candidateStorage.reset(new (std::nothrow) ParsedExternal{});
    }
    if (!candidateStorage) {
        return ExternalReadResult::lane2;
    }
    // Receive decoding does not re-enter on this thread. Reset its retained image per packet.
    ParsedExternal& candidate = *candidateStorage;
    std::destroy_at(&candidate);
    std::construct_at(&candidate);
    bool lanePresent = false;
    bool externalPresent = false;
    if (!reader.skip(bitOffset) || !read_flag(reader, externalPresent) || !externalPresent) {
        return ExternalReadResult::prefix;
    }
    if (!read_flag(reader, candidate.commonPresent)
        || (candidate.commonPresent
            && !middleware::gameplay::external::read_common_state(reader, candidate.common))) {
        return ExternalReadResult::common;
    }
    output.commonPresent = candidate.commonPresent;
    output.common = candidate.common;
    if (lane0Transport.write != nullptr) {
        if (!middleware::gameplay::external::read_simulation_event_lane(
                reader, lane0Transport.payloadCodec, candidate.lane0)) {
            return ExternalReadResult::lane0;
        }
    } else if (lane0.read != nullptr) {
        if (!lane0.read(lane0.context, reader)) {
            return ExternalReadResult::lane0;
        }
    } else if (!read_flag(reader, lanePresent) || lanePresent) {
        return ExternalReadResult::lane0;
    }
    if (!middleware::gameplay::external::read_control_state_lane(
            reader,
            middleware::gameplay::external::control_state_payload_codec(),
            candidate.lane1)) {
        return ExternalReadResult::lane1;
    }
    const auto lane2Offset = payload.size() * 8U - reader.remaining_bits();
    if (g_entityTransport.read != nullptr
            ? !g_entityTransport.read(g_entityTransport.context, source, reader, candidate.entities)
            : !middleware::gameplay::external::read_entity_batch(
                  reader, entities, candidate.entities)) {
        log_rejected_entity_packet(
            source, payload, bitOffset, lane2Offset, payload.size() * 8U - reader.remaining_bits());
        return ExternalReadResult::lane2;
    }
    if (!read_player_lane(reader)) {
        return ExternalReadResult::lane3;
    }
    wire::FillerTrailer filler{};
    if (!wire::read_filler_and_padding(reader, filler) || reader.remaining_bits() != 0) {
        return ExternalReadResult::filler;
    }
    output = candidate;
    return ExternalReadResult::accepted;
}

/** @return Stable log name for one external read result. */
[[nodiscard]] const char* external_result_name(ExternalReadResult result) noexcept {
    // One name per ExternalReadResult, in declaration order.
    constexpr std::array<const char*, 8> names = {
        "accepted", "prefix", "common", "lane0", "lane1", "lane2", "lane3", "filler"};
    const auto index = static_cast<std::size_t>(result);
    return index < names.size() ? names[index] : "unknown";
}

/** Queues message 44 after the peer transaction accepts the initial common root. */
void queue_common_request(const state::gameplay::Endpoint& from,
                          std::uint64_t groupSessionId,
                          const state::activity::SessionBinding& binding,
                          std::uint64_t ownerGeneration,
                          std::uint8_t requested) noexcept {
    if (!sunrise::server::bap::request_replication_epoch(binding, ownerGeneration, requested)) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* const peer = find_locked(from);
    if (peer != nullptr && peer->externalGroupSessionId == groupSessionId
        && peer->commonReconciler.owner_generation() == ownerGeneration) {
        static_cast<void>(peer->commonReconciler.commit_request());
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Writes the external handler body after both reliable queues; true when all of it fit. */
[[nodiscard]] bool write_external(const gp::PeerLink& peer, bits::Writer& writer) noexcept {
    middleware::gameplay::external::CommonState common{};
    const auto viewPhase = peer.viewReceptor.phase();
    if (peer.stage != gp::PeerStage::connected
        || (viewPhase != gp::external::view_receptor::Phase::provisional
            && viewPhase != gp::external::view_receptor::Phase::accepted)
        || peer.externalGroupSessionId == 0 || !peer.commonReconciler.outbound_common(common)
        || (g_lane0Transport.write == nullptr && g_lane0Codec.write == nullptr)) {
        return false;
    }
    const bool commonPresent = !peer.commonCommitted;
    if (!writer.write(commonPresent ? 1U : 0U, 1)
        || (commonPresent && !middleware::gameplay::external::write_common_state(writer, common))
        || (g_lane0Transport.write != nullptr
                ? !g_lane0Transport.write(g_lane0Transport.context,
                                          peer.externalGroupSessionId,
                                          peer.nextExternalTransmission,
                                          writer)
                : !g_lane0Codec.write(g_lane0Codec.context, writer))
        || !writer.write(0, 1) || !writer.write(0, 1) || !writer.write(0, 1) || !writer.write(1, 1)
        || !writer.write(0, 1)) {
        return false;
    }
    return true;
}

/** Exact acknowledgement carried by the measured and transmitted packet. */
[[nodiscard]] wire::AckState acknowledgement(const gp::PeerLink& peer) noexcept {
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
    return ack;
}

/** Builds and sends one ACK packet from a peer copy taken under the lock. */
[[nodiscard]] bool send_acknowledgement(const gp::PeerLink& peer) noexcept {
    const auto ack = acknowledgement(peer);
    std::array<std::byte, kReplyCapacity> buffer{};
    bits::Writer writer(buffer);
    const std::uint8_t guard = wire::connection_sequence_low2(peer.localConnectionSequence);
    std::size_t size = 0;
    // Only the 32-byte queue carries this host's messages; the 6-byte queue stays empty.
    middleware::gameplay::external::CommonState externalCommon{};
    const auto viewPhase = peer.viewReceptor.phase();
    const bool external = (viewPhase == gp::external::view_receptor::Phase::provisional
                           || viewPhase == gp::external::view_receptor::Phase::accepted)
                          && peer.externalGroupSessionId != 0
                          && peer.commonReconciler.outbound_common(externalCommon);
    if (!wire::write_head_and_ack(writer, guard, ack) || !wire::write_queue(writer, peer.outbound)
        || !wire::write_empty_queue(writer)) {
        return false;
    }
    if (external) {
        if (!writer.write(1, 1) || !write_external(peer, writer) || !writer.write(0, 1)) {
            return false;
        }
    } else if (!wire::write_absent_filler(writer)) {
        return false;
    }
    if (!writer.finish(size)) {
        return false;
    }
    // Keep the selected native channel alive through the transport send.
    // Transport sealing does not call the peer layer; association receive releases its lock first.
    AcquireSRWLockExclusive(&g_lock);
    auto* current = find_locked(peer.endpoint);
    const bool owned =
        current && current->peerGeneration == peer.peerGeneration
        && current->channelGeneration == peer.channelGeneration
        && current->viewGeneration == peer.viewGeneration
        && current->localConnectionSequence == peer.localConnectionSequence
        && current->remoteConnectionSequence == peer.remoteConnectionSequence
        && current->externalGroupSessionId == peer.externalGroupSessionId
        && current->activityBinding.sessionId == peer.activityBinding.sessionId
        && current->activityBinding.createdRevision == peer.activityBinding.createdRevision
        && current->commonReconciler.owner_generation() == peer.commonReconciler.owner_generation();
    const bool sent = owned && send_transport(peer.endpoint, {buffer.data(), size});
    ReleaseSRWLockExclusive(&g_lock);
    return sent;
}

} // namespace

/** Consumes one established packet. */
void consume_established(const gp::Endpoint& from,
                         std::span<const std::byte> payload,
                         std::uint64_t now) noexcept {
    PacketInput input{};
    if (!prepare_packet_input(from, payload, now, input)) {
        return;
    }
    payload = input.payload;
    wire::EstablishedPacket packet{};
    if (!wire::decode_established(payload, true, packet)) {
        report(core::log::Level::debug, "ev=gameplay stage=packet result=drop reason=grammar");
        return;
    }
    std::array<std::uint8_t, kMessageReportCapacity> delivered{};
    std::size_t deliveredCount = 0;
    unsigned stage = 0;
    bool queueCleared = false;
    std::uint16_t clearedPacket = 0;
    std::uint64_t externalGroupSessionId = 0;
    // The reliable window never resynchronises, so a stalled queue is only visible as a refused
    // record against the sequence it is still waiting for.
    std::size_t largeDropped = 0;
    std::uint16_t largeNext = 0;
    std::uint16_t largeFirst = 0;
    bool peerFound = false;
    bool guardAccepted = false;
    bool externalExpected = false;
    bool externalValid = true;
    const char* externalFailure = "none";
    thread_local std::unique_ptr<ParsedExternal> externalStorage;
    if (!externalStorage) {
        externalStorage.reset(new (std::nothrow) ParsedExternal{});
    }
    if (!externalStorage) {
        return;
    }
    ParsedExternal& external = *externalStorage;
    std::destroy_at(&external);
    std::construct_at(&external);
    state::activity::SessionBinding commonBinding{};
    std::uint64_t commonOwnerGeneration = 0;
    std::uint8_t commonRequestedGeneration = 0;
    bool commonRequest = false;
    gp::external::common_reconciler::Reconciler commonCandidate{};
    bool commonCandidatePresent = false;
    DisplacedExternals completed{};
    std::size_t completedCount = 0;
    std::uint8_t expectedGuard = 0;
    std::array<wire::AssembledMessage, kMessageReportCapacity> bodies{};
    std::array<bool, kMessageReportCapacity> deferredView{};
    gp::entity_identity::Source ingress{};
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* peer = find_locked(from);
    if (peer != nullptr) {
        ingress = entity_source(*peer);
        expectedGuard = wire::connection_sequence_low2(peer->remoteConnectionSequence);
        guardAccepted =
            packet.connectionSequenceLow2 == expectedGuard && packet_channel_matches(*peer, input);
    }
    if (guardAccepted) {
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
    // Reliable controls establish the view used by this packet's external payload.
    if (guardAccepted) {
        bind_participant(from, ingress);
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
        bits::Reader reader({body.bytes.data(), gp::kReassemblyCapacity});
        namespace viewWire = middleware::gameplay::group;
        if (body.id == viewWire::kViewMessageId) {
            auto stageReader = reader;
            viewWire::ViewEstablishment transition{};
            if (stageReader.skip(body.bodyBitOffset) && viewWire::read_view(stageReader, transition)
                && transition.kind == 5) {
                // Stage 5 may depend on common state carried later in this same packet.
                deferredView[index] = true;
                continue;
            }
        }
        if (reader.skip(body.bodyBitOffset) && !group::consume(from, body.id, reader, now)) {
            report(core::log::Level::debug,
                   "ev=gameplay stage=message result=undecoded id=%u",
                   static_cast<unsigned>(body.id));
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    peer = find_locked(from);
    const bool sameChannel = peer != nullptr && peer->peerGeneration == ingress.peerGeneration
                             && peer->channelGeneration == ingress.channelGeneration
                             && peer->localConnectionSequence == ingress.localConnectionSequence
                             && peer->remoteConnectionSequence == ingress.remoteConnectionSequence;
    guardAccepted = guardAccepted && sameChannel;

    if (peer != nullptr) {
        peerFound = true;
        expectedGuard = wire::connection_sequence_low2(peer->remoteConnectionSequence);
        guardAccepted =
            guardAccepted && sameChannel && packet.connectionSequenceLow2 == expectedGuard;
        externalExpected = peer->viewReceptor.accepts_inbound_entities();
        if (guardAccepted && externalExpected) {
            externalGroupSessionId = peer->externalGroupSessionId;
            const auto source = entity_source(*peer);
            const auto ordinal = packet_ordinal(*peer, packet.ack.outboundHead);
            const ExternalReadResult externalRead = read_external(payload,
                                                                  packet.externalBitOffset,
                                                                  source,
                                                                  g_lane0Codec,
                                                                  g_lane0Transport,
                                                                  g_entityCodec,
                                                                  external);
            externalValid = externalRead == ExternalReadResult::accepted;
            externalFailure = external_result_name(externalRead);
            if (external.commonPresent) {
                commonCandidate = peer->commonReconciler;
                const auto result = commonCandidate.observe(external.common);
                const bool commonValid =
                    result == gp::external::common_reconciler::ObserveResult::initialAccepted
                    || result
                           == gp::external::common_reconciler::ObserveResult::
                               awaitingRequestedGeneration
                    || result == gp::external::common_reconciler::ObserveResult::ready;
                if (!commonValid) {
                    externalValid = false;
                    externalFailure = "reconcile";
                }
                commonRequest =
                    result == gp::external::common_reconciler::ObserveResult::initialAccepted
                    && commonCandidate.pending_request(commonRequestedGeneration);
                if (commonRequest) {
                    commonBinding = peer->activityBinding;
                    commonOwnerGeneration = commonCandidate.owner_generation();
                }
                commonCandidatePresent = commonValid;
                if (commonValid) {
                    static_cast<void>(commonCandidate.qualify_entities(
                        &external.common, ordinal, packet.ack.outboundHeadPresent));
                    peer->commonReconciler = commonCandidate;
                }
            }
            if (externalValid) {
                if (!commonCandidatePresent) {
                    commonCandidate = peer->commonReconciler;
                }
                const bool currentEntityEpoch = commonCandidate.qualify_entities(
                    external.commonPresent ? &external.common : nullptr,
                    ordinal,
                    packet.ack.outboundHeadPresent);
                if (external.entities.recordPresent && !currentEntityEpoch) {
                    externalValid = false;
                    externalFailure = "entity_epoch";
                } else {
                    external.entities.hasAllocationEpoch = currentEntityEpoch;
                    external.entities.allocationEpoch = commonCandidate.requested_generation();
                    external.entities.allocationDomain = commonCandidate.allocation_domain();
                    commonCandidatePresent = true;
                }
            }
            middleware::gameplay::external::EntityBaselineMutation entityMutation{};
            if (externalValid && external.entities.recordPresent
                && g_entityTransport.prepare != nullptr) {
                externalValid = g_entityTransport.commit != nullptr
                                && g_entityTransport.prepare(g_entityTransport.context,
                                                             source,
                                                             external.entities,
                                                             packet.ack.outboundHead,
                                                             packet.ack.outboundHeadPresent,
                                                             ordinal,
                                                             entityMutation);
                if (!externalValid) {
                    externalFailure = "lane2_prepare";
                }
            }
            if (externalValid && externalGroupSessionId != 0
                && g_lane0Transport.accepted != nullptr) {
                externalValid = g_lane0Transport.accepted(
                    g_lane0Transport.context, externalGroupSessionId, external.lane0);
                if (!externalValid) {
                    externalFailure = "lane0_accept";
                }
            }
            // The peer lock keeps a prepared baseline stable until commit.
            if (externalValid && externalGroupSessionId != 0 && external.entities.recordPresent) {
                externalValid = g_entityTransport.commit != nullptr
                                    ? g_entityTransport.commit(
                                          g_entityTransport.context, source, entityMutation)
                                    : g_entityAccepted == nullptr
                                          || g_entityAccepted(g_entityAcceptedContext,
                                                              externalGroupSessionId,
                                                              external.entities);
                if (!externalValid) {
                    externalFailure = "lane2_accept";
                }
            }
            if (externalValid && commonCandidatePresent) {
                peer->commonReconciler = commonCandidate;
            }
            if (externalValid && external.entities.recordPresent
                && g_entityTransport.observed != nullptr) {
                external.entities.ignoredRecordMask = entityMutation.ignoredRecordMask;
                g_entityTransport.observed(g_entityTransport.context,
                                           source,
                                           external.entities,
                                           packet.ack.outboundHead,
                                           packet.ack.outboundHeadPresent,
                                           ordinal,
                                           now);
            }
        }
    }
    // Authenticated transport receipts remain valid when an inbound application lane is refused.
    if (guardAccepted) {
        for (auto& contribution : peer->externalContributions) {
            if (!contribution.occupied) {
                continue;
            }
            const wire::AckOutcome outcome =
                wire::acknowledgement_outcome(packet.ack, contribution.packetSequence);
            if (outcome == wire::AckOutcome::unresolved) {
                continue;
            }
            completed[completedCount++] = {
                contribution.groupSessionId, contribution.transmissionId, outcome};
            if (outcome == wire::AckOutcome::received && contribution.commonPresent
                && contribution.viewGeneration == peer->viewGeneration) {
                peer->commonCommitted = true;
            }
            contribution = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!peerFound) {
        return;
    }
    if (!guardAccepted) {
        report(core::log::Level::debug,
               "ev=gameplay stage=packet result=drop reason=%s got=%u expect=%u",
               sameChannel ? "channel_low2" : "channel_changed",
               static_cast<unsigned>(packet.connectionSequenceLow2),
               static_cast<unsigned>(expectedGuard));
        return;
    }
    if (!externalValid) {
        report(core::log::Level::debug,
               "ev=gameplay stage=external result=drop reason=%s group=0x%016llX",
               externalFailure,
               static_cast<unsigned long long>(externalGroupSessionId));
    }
    if (commonRequest && externalGroupSessionId != 0) {
        queue_common_request(from,
                             externalGroupSessionId,
                             commonBinding,
                             commonOwnerGeneration,
                             commonRequestedGeneration);
    }
    for (std::size_t index = 0; index < deliveredCount; ++index) {
        if (!deferredView[index]) {
            continue;
        }
        const auto& body = bodies[index];
        bits::Reader reader({body.bytes.data(), gp::kReassemblyCapacity});
        if (reader.skip(body.bodyBitOffset)) {
            static_cast<void>(group::consume(from, body.id, reader, now));
        }
    }
    notify_external_outcomes(completed, completedCount);
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

void service(std::uint64_t now) noexcept {
    std::unique_lock sendGuard(g_sendLock, std::try_to_lock);
    if (!sendGuard.owns_lock()) {
        return;
    }
    const auto& owed = g_sendSnapshot;
    for (std::size_t index = 0; index < g_peers.size(); ++index) {
        AcquireSRWLockExclusive(&g_lock);
        auto& peer = g_peers[index];
        // An unacknowledged send queue keeps the packet going out until the peer confirms it.
        // Every packet burns one sequence, so the resend is paced.
        const bool resendDue = peer.outbound.count != 0 && now - peer.lastSend >= kResendInterval;
        const bool due = peer.acknowledgementOwed || resendDue;
        if (peer.stage == gp::PeerStage::absent || !due) {
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        middleware::gameplay::external::CommonState externalCommon{};
        const auto viewPhase = peer.viewReceptor.phase();
        const bool external = (viewPhase == gp::external::view_receptor::Phase::provisional
                               || viewPhase == gp::external::view_receptor::Phase::accepted)
                              && peer.externalGroupSessionId != 0
                              && peer.commonReconciler.outbound_common(externalCommon);
        const std::uint16_t nextPacket =
            static_cast<std::uint16_t>((peer.outboundHead + 1) % gp::kPacketSequenceModulus);
        if (external
            && peer.externalContributions[nextPacket % peer.externalContributions.size()]
                   .occupied) {
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        peer.acknowledgementOwed = false;
        peer.lastSend = now;
        // Later enqueues wait behind this run, preserving the current acknowledgement target.
        wire::outbound_window::begin(peer.outbound, nextPacket);
        // The packet sequence advances here so the copy carries the value it will publish.
        peer.outboundHead = nextPacket;
        peer.outboundHeadPresent = true;
        if (external) {
            ++peer.nextExternalTransmission;
            if (peer.nextExternalTransmission == 0) {
                ++peer.nextExternalTransmission;
            }
            auto& contribution =
                peer.externalContributions[nextPacket % peer.externalContributions.size()];
            contribution.transmissionId = peer.nextExternalTransmission;
            contribution.groupSessionId = peer.externalGroupSessionId;
            contribution.viewGeneration = peer.viewGeneration;
            contribution.packetSequence = nextPacket;
            contribution.commonPresent = !peer.commonCommitted;
            contribution.lane0Present = true;
            contribution.occupied = true;
        }
        peer.lastTick = now;
        g_sendSnapshot = peer;
        ReleaseSRWLockExclusive(&g_lock);
        if (send_acknowledgement(owed)) {
            continue;
        }
        report(core::log::Level::debug, "ev=gameplay stage=ack result=fail");
        DisplacedExternals displaced{};
        std::size_t displacedCount = 0;
        AcquireSRWLockExclusive(&g_lock);
        gp::PeerLink* const current = find_locked(owed.endpoint);
        if (current != nullptr && current->peerGeneration == owed.peerGeneration
            && current->channelGeneration == owed.channelGeneration
            && current->localConnectionSequence == owed.localConnectionSequence
            && current->remoteConnectionSequence == owed.remoteConnectionSequence) {
            current->acknowledgementOwed = true;
            wire::outbound_window::send_failed(current->outbound, owed.outboundHead);
            auto& reserved =
                current->externalContributions[owed.outboundHead
                                               % current->externalContributions.size()];
            // The packet never left, so the stake is displaced like any other lost contribution.
            if (reserved.occupied && reserved.transmissionId == owed.nextExternalTransmission) {
                displaced[displacedCount++] = {
                    reserved.groupSessionId, reserved.transmissionId, wire::AckOutcome::unresolved};
                reserved = {};
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        notify_external_outcomes(displaced, displacedCount);
    }
}

} // namespace sunrise::server::gameplay::peer
