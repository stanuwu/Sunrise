#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/gameplay/definition.h"
#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::gameplay::peer {

/** The acknowledgement history covers the eight newest packets. */
inline constexpr std::size_t kAckHistory = 8;
/** The outbound packet ring holds 128 entries, so the acknowledged base is 7 bits. */
inline constexpr std::size_t kPacketRingSize = 128;
/** The larger reliable queue uses 32-byte fragments. */
inline constexpr std::size_t kLargeFragmentBytes = 32;
/** The smaller reliable queue uses 6-byte fragments. */
inline constexpr std::size_t kSmallFragmentBytes = 6;
/** One packet carries at most this many queue records before it is refused. */
inline constexpr std::size_t kMaximumRecords = 64;
/** The two-bit connection guard is the full connection sequence modulo four. */
inline constexpr std::uint32_t kConnectionSequenceGuardModulus = 4;

/**
 * Reduces a full connection sequence to the guard carried by an established packet.
 * @param sequence Full connection sequence announced during connect.
 * @return The sequence modulo four.
 */
[[nodiscard]] constexpr std::uint8_t connection_sequence_low2(std::uint32_t sequence) noexcept {
    return static_cast<std::uint8_t>(sequence % kConnectionSequenceGuardModulus);
}

/** Acknowledgement state this side publishes and the other side reads. */
struct AckState {
    /** Sequence of our newest outbound packet, or absent when we have sent none. */
    std::uint16_t outboundHead{};
    bool outboundHeadPresent{};
    /** Distance from the outbound head back to the event cursor. */
    std::uint8_t headMinusCursor{};
    /** Sequence of the newest packet received from the peer. */
    std::uint16_t receiveHead{};
    /** True once one packet has been received, which selects the initialized form. */
    bool ringInitialized{};
    /** Entry `i` is the packet `i + 1` before the head. The head itself carries no entry. */
    std::array<bool, kAckHistory> received{};
    /** Entries the wire form actually named. The rest of the array is unreported, not clear. */
    std::uint8_t reportedCount{};
    /** Half the measured delay, or the 1,023 sentinel. */
    std::uint16_t delay{};
};

/** One reliable-queue record taken off the wire. */
struct QueueRecord {
    /** Unwrapped message sequence. */
    std::uint16_t sequence{};
    /** True for the shorter final fragment of a message. */
    bool shortFragment{};
    /** Fragment payload bits. A fixed fragment carries the queue's whole fragment size. */
    std::uint16_t bitCount{};
    std::array<std::byte, kLargeFragmentBytes> bytes{};
};

/** Every record one packet carried, in wire order. */
struct QueueRecords {
    std::array<QueueRecord, kMaximumRecords> records{};
    std::size_t count{};
};

/** One decoded established packet. */
struct EstablishedPacket {
    std::uint8_t connectionSequenceLow2{};
    AckState ack{};
    /** Records from the 32-byte queue. */
    QueueRecords large{};
    /** Records from the 6-byte queue. */
    QueueRecords small{};
    /** Bit offset of the external gameplay handler body, once the view gate has opened. */
    std::size_t externalBitOffset{};
    bool hasExternal{};
};

/** Bounded filler trailer after the external handler. */
struct FillerTrailer {
    std::size_t bitCount{};
    bool present{};
};

/**
 * Decodes one established packet up to and including both reliable queues.
 * The external handler body is not parsed here. The packet reports the bit offset it starts at.
 * @param payload Whole decrypted transport payload.
 * @param expectExternal True once one external handler is registered on this connection.
 * @param output Receives the decoded packet.
 * @return True when every field of the fixed handler run was present and in range.
 */
[[nodiscard]] bool decode_established(std::span<const std::byte> payload,
                                      bool expectExternal,
                                      EstablishedPacket& output) noexcept;

/**
 * Writes the packet head and the acknowledgement handler payload.
 * @param writer Writer positioned at the start of the payload.
 * @param connectionSequenceLow2 Local connection sequence modulo four.
 * @param ack Acknowledgement state to publish.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_head_and_ack(encoding::bits::Writer& writer,
                                      std::uint8_t connectionSequenceLow2,
                                      const AckState& ack) noexcept;

/**
 * Reports whether one acknowledgement covers a packet this host sent.
 * The base names the newest packet the peer holds. Anything older than its named entries counts
 * as delivered.
 * @param ack Acknowledgement state the peer published.
 * @param sentSequence Sequence of the packet to test.
 * @return True when the peer has that packet.
 */
[[nodiscard]] bool acknowledgement_covers(const AckState& ack, std::uint16_t sentSequence) noexcept;

/**
 * Writes one reliable queue that carries no records.
 * @param writer Writer positioned at that queue's payload.
 * @return True when the terminator fit.
 */
[[nodiscard]] bool write_empty_queue(encoding::bits::Writer& writer) noexcept;

/**
 * Writes one reliable queue and every fragment it owes.
 * Each record names its sequence absolutely. The delta forms desynchronise if a record is dropped.
 * @param writer Writer positioned at that queue's payload.
 * @param queue Fragments to write, in sequence order.
 * @return True when the whole list and its terminator fit.
 */
[[nodiscard]] bool write_queue(encoding::bits::Writer& writer,
                               const state::gameplay::OutboundQueue& queue) noexcept;

/**
 * Splits one reliable message into fragments and appends them to a send queue.
 * @param queue Queue receiving the fragments.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares for that id.
 * @param body Encoded message body bits, without the inner header.
 * @param bodyBits Number of meaningful bits in the body.
 * @return True when the whole message fit the queue.
 */
[[nodiscard]] bool enqueue_message(state::gameplay::OutboundQueue& queue,
                                   std::uint8_t id,
                                   std::uint32_t declaredSize,
                                   std::span<const std::byte> body,
                                   std::size_t bodyBits) noexcept;

/** Reads the filler and requires exact zero byte padding. */
[[nodiscard]] bool read_filler_and_padding(encoding::bits::Reader& reader,
                                           FillerTrailer& output) noexcept;

/**
 * Writes the filler trailer that ends every packet.
 * @param writer Writer positioned after the last handler payload.
 * @return True when the absent-filler bit fit.
 */
[[nodiscard]] bool write_absent_filler(encoding::bits::Writer& writer) noexcept;

} // namespace sunrise::middleware::gameplay::peer
