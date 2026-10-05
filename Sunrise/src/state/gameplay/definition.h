#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../core/network_capacity.h"
#include "../activity/definition.h"
#include "external/definition.h"
#include "external/replication_common_reconciler.h"
#include "external/replication_view_receptor.h"

namespace sunrise::state::gameplay {

/** The protected envelope keys AES-128. */
inline constexpr std::size_t kChannelKeySize = 16;
/** A serialized NetAddr, the form every peer message carries an address in. */
inline constexpr std::size_t kNetAddrBlobSize = 86;
/** The exchanged nonce seed is 12 bytes and is also the nonce width. */
inline constexpr std::size_t kNonceSeedSize = 12;
/** Direct gameplay packets carry a two-byte proxy-address trailer. */
inline constexpr std::size_t kAddressTrailerSize = 2;
/** One public activity holds few direct associations, and every slot is fixed storage. */
inline constexpr std::size_t kAssociationCapacity = core::network_capacity::kConnections;
/** An offer that never completes is torn down after this many milliseconds. */
inline constexpr std::uint64_t kAssociationTimeoutMs = 10'000;

/** How far one direct association has progressed. */
enum class AssociationStage : std::uint8_t {
    /** The slot is free. */
    absent,
    /** An offer was accepted and its response was sent. Nothing is protected yet. */
    pending,
    /** Key material is installed and protected datagrams are accepted. */
    established,
    /** The association failed and its slot is waiting to be reclaimed. */
    failed,
};

/** One IPv4 endpoint in host byte order. */
struct Endpoint {
    std::uint32_t address{};
    std::uint16_t port{};
    /**
     * Host port the peer dialled, which is what separates two links from one peer.
     * The client sends every channel from one source port, so the peer address alone names them
     * all. Zero means the primary host port.
     */
    std::uint16_t localPort{};

    /** Every field names the endpoint, the local port included. */
    [[nodiscard]] bool operator==(const Endpoint&) const noexcept = default;
};

/**
 * Crypto state installed in one step when an association becomes established.
 * The two nonce bases are different values: the outbound base is generated here and the
 * inbound base arrives in the offer. Nothing may alias them.
 */
struct ProtectedContext {
    std::array<std::byte, kChannelKeySize> key{};
    std::array<std::byte, kNonceSeedSize> outboundBase{};
    std::array<std::byte, kNonceSeedSize> inboundBase{};
    /** Direct-address trailer mirrored outside and inside each protected packet. */
    std::array<std::byte, kAddressTrailerSize> addressTrailer{};
    /** Advanced before every protected send so no two sends share a nonce. */
    std::uint32_t outboundWordA{};
    /** Stable for the association's lifetime. */
    std::uint32_t outboundWordB{};
    bool installed{};
};

/** One direct association with a client endpoint. */
struct Association {
    Endpoint endpoint{};
    ProtectedContext protection{};
    AssociationStage stage{AssociationStage::absent};
    /** Word A of the newest accepted offer. An older or equal offer is refused. */
    std::uint32_t offerWordA{};
    /** Word B of the accepted offer. The response echoes exactly this value. */
    std::uint32_t offerWordB{};
    /** True once an offer with this word B has been answered. */
    bool answered{};
    /** Highest word A accepted from a protected packet. */
    std::uint32_t acceptedWordA{};
    /** True once one protected packet has been accepted. */
    bool acceptedAny{};
    std::uint64_t createdTick{};
    std::uint64_t lastTick{};
};

/** The acknowledgement history covers the eight newest received packets. */
inline constexpr std::size_t kAckHistory = 8;
/** The packet sequence the acknowledgement header publishes is ten bits wide. */
inline constexpr std::uint16_t kPacketSequenceModulus = 1024;
/** A packet more than half the sequence space behind the head is old, not new. */
inline constexpr std::uint16_t kPacketSequenceHalf = kPacketSequenceModulus / 2;

/** A view list carries at most 16 bytes. */
inline constexpr std::size_t kViewListCapacity = 16;

/**
 * View signature bound for one peer.
 * Entity output is refused until this is bound, because a mismatched signature makes the peer
 * discard every contribution.
 */
struct ViewSignature {
    std::array<std::byte, kViewListCapacity> list{};
    std::uint64_t token{};
    /** Optional value the sender carried. Its meaning is unrecovered, so it is kept, not read. */
    std::int32_t optionalValue{};
    std::uint8_t kind{};
    std::uint8_t listCount{};
    bool hasOptionalValue{};
    bool hasList{};
    bool bound{};
};

/** Peer connection stage. The numbers are the engine's own. Do not renumber them. */
enum class PeerStage : std::uint8_t {
    absent = 0,
    allocated = 1,
    teardown = 2,
    connecting = 3,
    establishing = 4,
    connected = 5,
};

/** Reliable message sequences unwrap modulo 8,192. */
inline constexpr std::uint16_t kMessageSequenceModulus = 8192;
/**
 * One queue buffers this many out-of-order fragments before it drops the newest.
 * The depth is a chosen bound, several send windows deep, and overflow drops rather than overruns.
 */
inline constexpr std::size_t kReliableSlots = 64;
/** The larger reliable queue carries 32-byte fragments, which bounds one slot. */
inline constexpr std::size_t kReliableFragmentBytes = 32;
/** Local body ceiling for membership composition; encoding a larger snapshot fails. */
inline constexpr std::size_t kGroupMessageCapacity = 8192;
/** Chosen staging/receive headroom above that ceiling, including the inner message header. */
inline constexpr std::size_t kGroupMessageHeaderRoom = 512;
/** Local receive bound; an oversized fragment run is discarded through its terminator. */
inline constexpr std::size_t kReassemblyCapacity = kGroupMessageCapacity + kGroupMessageHeaderRoom;

/** One buffered reliable fragment. */
struct ReliableFragment {
    std::array<std::byte, kReliableFragmentBytes> bytes{};
    std::uint16_t sequence{};
    std::uint16_t bitCount{};
    /** True for the shorter fragment that ends a message. */
    bool shortFragment{};
    bool occupied{};
};

/** One reliable receive queue and the sequence it is waiting for. */
struct ReliableQueue {
    std::array<ReliableFragment, kReliableSlots> fragments{};
    std::uint16_t nextSequence{};
    /** Contiguous fragments retire into this bounded message across packet acknowledgements. */
    std::array<std::byte, kReassemblyCapacity> assembly{};
    std::size_t assemblyBits{};
    /** Oversized or malformed runs are consumed through their terminator. */
    bool discarding{};
    /** False until the first record arrives, which is what fixes the starting sequence. */
    bool started{};
};

/** Local burst storage; a message that cannot fit is refused without partially enqueueing it. */
inline constexpr std::size_t kOutboundSlots = 512;
/** 16 KiB of fragment payload; headers and terminators also consume this space. */
static_assert(kOutboundSlots * kReliableFragmentBytes == 2 * kGroupMessageCapacity);
/** Local pacing choice: at most 256 bytes of queued fragments per packet, before wire headers. */
inline constexpr std::size_t kOutboundSendWindow = 8;
/** Outgoing staging includes the inner header; receive assembly keeps its own bound. */
inline constexpr std::size_t kOutboundMessageCapacity =
    kGroupMessageCapacity + kGroupMessageHeaderRoom;

/** One reliable fragment this host owes the peer. */
struct OutboundFragment {
    std::array<std::byte, kReliableFragmentBytes> bytes{};
    std::uint16_t sequence{};
    std::uint16_t bitCount{};
    /** True for the shorter fragment that ends a message. */
    bool shortFragment{};
    bool occupied{};
};

/** Sequence the peer expects the first reliable record of a connection to carry. */
inline constexpr std::uint16_t kFirstMessageSequence = 1;

/**
 * Group sessions one link carries at once. Two public regions overlap during a transition. The
 * rest is headroom.
 */
inline constexpr std::size_t kSessionsPerLink = 4;

/** One reliable send queue. */
struct OutboundQueue {
    std::array<OutboundFragment, kOutboundSlots> fragments{};
    /** Sequence the next enqueued fragment takes. The peer refuses a first record that is not 1. */
    std::uint16_t nextSequence{kFirstMessageSequence};
    std::size_t count{};
    /** First fragments awaiting acknowledgement; later enqueues are outside this run. */
    std::size_t carriedCount{};
    /** Packet sequence the carried run was first written into. A resend keeps it. */
    std::uint16_t sentInPacket{};
    /** True while that packet is unacknowledged, which is what drives the resend. */
    bool awaitingAcknowledgement{};
};

/** One peer connection over an established association. */
struct PeerLink {
    Endpoint endpoint{};
    PeerStage stage{PeerStage::absent};
    /** Sequences this side chose when it answered the connect request. */
    std::uint32_t localConnectionSequence{};
    std::uint32_t localTransportSequence{};
    /** Sequences the connect request carried. */
    std::uint32_t remoteConnectionSequence{};
    std::uint32_t remoteTransportSequence{};
    /**
     * Group sessions joined over this link, zero in the free slots. The client holds one channel
     * per host peer and multiplexes every region's session over it, so a link carries a set.
     */
    std::array<std::uint64_t, kSessionsPerLink> sessions{};
    /** The peer's own NetAddr, taken from its connect request. */
    std::array<std::byte, kNetAddrBlobSize> remoteAddress{};
    /** True once that address has been captured. */
    bool remoteAddressPresent{};
    /** Newest packet sequence received from this peer. */
    std::uint16_t receiveHead{};
    /** Expanded receive sequence orders sparse entity updates across packet wraps. */
    std::uint64_t receiveOrdinal{};
    bool ringInitialized{};
    /** Entry `i` is the packet `i + 1` before the head. The head itself carries no entry. */
    std::array<bool, kAckHistory> received{};
    /** Sequence of the newest packet sent to this peer. */
    std::uint16_t outboundHead{};
    bool outboundHeadPresent{};
    /** Fragments from the 32-byte queue. */
    ReliableQueue large{};
    /** Fragments from the 6-byte queue. */
    ReliableQueue small{};
    /** Fragments this host owes the peer on the 32-byte queue. */
    OutboundQueue outbound{};
    /** View signature bound for this peer. Replication is refused until it is bound. */
    ViewSignature view{};
    /** Process-local identity changed whenever this endpoint rebuilds its channel. */
    std::uint64_t peerGeneration{};
    std::uint64_t channelGeneration{};
    std::uint64_t viewGeneration{};
    external::view_receptor::Receptor viewReceptor{};
    external::common_reconciler::Reconciler commonReconciler{};
    activity::SessionBinding activityBinding{};
    /** Session that owns the active external view and its sessionless lane bodies. */
    std::uint64_t externalGroupSessionId{};
    /** True after an ordinary packet outcome covers the current common root. */
    bool commonCommitted{};
    external::ExternalShadow externalShadow{};
    std::array<external::ExternalContributionSnapshot, external::kExternalContributionCapacity>
        externalContributions{};
    std::uint64_t nextExternalTransmission{};
    /** True while a received packet still has to be acknowledged. */
    bool acknowledgementOwed{};
    /** The first reliable record of this channel incarnation has been queued. */
    bool establishQueued{};
    std::uint64_t lastTick{};
    /** Tick the last packet left at. The resend is paced against it. */
    std::uint64_t lastSend{};
};

/** Every direct association this process owns. */
struct GameplayState {
    std::array<Association, kAssociationCapacity> associations{};
    std::array<PeerLink, kAssociationCapacity> peers{};
    /** True once the endpoint is bound and the descriptor may be advertised. */
    bool endpointBound{};
    /** Endpoint actually bound, or zero in external topology. */
    Endpoint bound{};
};

} // namespace sunrise::state::gameplay
