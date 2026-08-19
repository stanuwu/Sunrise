#include "established_packet.h"

#include <array>

#include "../../encoding/bit_raw.h"
#include "peer_container.h"

namespace sunrise::middleware::gameplay::peer {

namespace {

namespace bits = encoding::bits;

/** An established packet carries marker 0. */
constexpr std::uint64_t kEstablishedMarker = 0;
/** Width of the marker and of the fragmented flag. */
constexpr std::uint8_t kFlagWidth = 1;
/** The connection sequence guard is two bits. */
constexpr std::uint8_t kSequenceGuardWidth = 2;
/** The outbound packet head is published as ten low bits. */
constexpr std::uint8_t kOutboundHeadWidth = 10;
/** The distance from the head back to the event cursor is seven bits. */
constexpr std::uint8_t kCursorWidth = 7;
/** The acknowledged base is seven bits, matching the 128-entry ring. */
constexpr std::uint8_t kAckBaseWidth = 7;
/** Every status form is selected by a two-bit prefix, one of which extends to three. */
constexpr std::uint8_t kStatusPrefixWidth = 2;
/** The extending bit of the three-bit prefixes. */
constexpr std::uint8_t kStatusExtensionWidth = 1;
/** Prefix of the empty-ring form. */
constexpr std::uint64_t kStatusEmpty = 0;
/** Prefix of the single-difference form. */
constexpr std::uint64_t kStatusSingle = 1;
/** Prefix of the eight-entry explicit form. */
constexpr std::uint64_t kStatusExplicit = 2;
/** Prefix shared by the long-span and uninitialized forms. */
constexpr std::uint64_t kStatusExtended = 3;
/** Extension bit selecting the long-span form. */
constexpr std::uint64_t kStatusLongSpan = 0;
/** Width of the long-span count. */
constexpr std::uint8_t kStatusCountWidth = 7;
/** Width of the index in the single-difference form. */
constexpr std::uint8_t kSingleIndexWidth = 3;
/** The acknowledgement delay is ten bits. */
constexpr std::uint8_t kDelayWidth = 10;
/** Longest status run the long-span form can name. */
constexpr std::uint64_t kMaximumStatusCount = 128;
/** The message sequence selector is two bits. */
constexpr std::uint8_t kSelectorWidth = 2;
/** Selector ending the record list. */
constexpr std::uint64_t kSelectorEnd = 0;
/** Selector introducing an absolute low-13 sequence. */
constexpr std::uint64_t kSelectorAbsolute = 1;
/** Selector introducing a four-bit delta above the previous sequence. */
constexpr std::uint64_t kSelectorDelta = 2;
/** Width of an absolute message sequence. */
constexpr std::uint8_t kAbsoluteSequenceWidth = 13;
/** Message sequences unwrap modulo 8,192. */
constexpr std::uint16_t kSequenceModulus = 8192;
/** Width of the delta form. */
constexpr std::uint8_t kDeltaWidth = 4;
/** Width of the queue-specific short length for the 32-byte queue. */
constexpr std::uint8_t kLargeShortLengthWidth = 8;
/** Width of the queue-specific short length for the 6-byte queue. */
constexpr std::uint8_t kSmallShortLengthWidth = 6;
/** Bits in one byte. */
constexpr std::uint16_t kByteBits = 8;
/** The reassembled message begins with a 6-bit registry id. */
constexpr std::uint8_t kMessageIdWidth = 6;
/** The declared decoded size after it is 18 bits. */
constexpr std::uint8_t kMessageSizeWidth = 18;
/** The filler length is a 14-bit field. */
constexpr std::uint8_t kFillerLengthWidth = 14;
/** The recovered filler body limit is 9,920 bits. */
constexpr std::uint64_t kMaximumFillerBits = 9920;
/** Byte padding can carry at most seven bits. */
constexpr std::size_t kMaximumPaddingBits = 7;

/**
 * Reads one ternary packet status.
 * @param reader Open reader.
 * @param received Receives true for the two codes that mean received.
 * @return True when a complete code was present.
 */
[[nodiscard]] bool read_status(bits::Reader& reader, bool& received) noexcept {
    std::uint64_t first = 0;
    if (!reader.read(kFlagWidth, first)) {
        return false;
    }
    if (first == 0) {
        // Code 0 is status 1, the ordinary received case.
        received = true;
        return true;
    }
    std::uint64_t second = 0;
    if (!reader.read(kFlagWidth, second)) {
        return false;
    }
    // Code 10 is status 0, unresolved. Code 11 is status 2, received out of order.
    received = second != 0;
    return true;
}

/** Writes one ternary packet status. @param received Selects status 1 or status 0. */
[[nodiscard]] bool write_status(bits::Writer& writer, bool received) noexcept {
    if (received) {
        return writer.write(0, kFlagWidth);
    }
    return writer.write(1, kFlagWidth) && writer.write(0, kFlagWidth);
}

/**
 * Reads the acknowledgement handler payload.
 * @param reader Open reader positioned after the packet head.
 * @param output Receives the peer's acknowledgement state.
 * @return True when the selected status form was complete.
 */
[[nodiscard]] bool read_ack(bits::Reader& reader, AckState& output) noexcept {
    std::uint64_t present = 0;
    std::uint64_t head = 0;
    std::uint64_t cursor = 0;
    if (!reader.read(kFlagWidth, present) || !reader.read(kOutboundHeadWidth, head)
        || !reader.read(kCursorWidth, cursor)) {
        return false;
    }
    output.outboundHeadPresent = present != 0;
    output.outboundHead = static_cast<std::uint16_t>(head);
    output.headMinusCursor = static_cast<std::uint8_t>(cursor);

    std::uint64_t base = 0;
    std::uint64_t prefix = 0;
    if (!reader.read(kAckBaseWidth, base) || !reader.read(kStatusPrefixWidth, prefix)) {
        return false;
    }
    output.receiveHead = static_cast<std::uint16_t>(base);
    output.received = {};
    output.reportedCount = 0;
    if (prefix == kStatusExtended) {
        std::uint64_t extension = 0;
        if (!reader.read(kStatusExtensionWidth, extension)) {
            return false;
        }
        if (extension != kStatusLongSpan) {
            // The uninitialized form ends the payload and carries no delay.
            output.ringInitialized = false;
            return true;
        }
        std::uint64_t count = 0;
        // The field is the number of statuses, not one less than it.
        if (!reader.read(kStatusCountWidth, count) || count > kMaximumStatusCount) {
            return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            bool received = false;
            if (!read_status(reader, received)) {
                return false;
            }
            if (index < output.received.size()) {
                output.received[index] = received;
                output.reportedCount = static_cast<std::uint8_t>(index + 1);
            }
        }
    } else if (prefix == kStatusExplicit) {
        for (bool& entry : output.received) {
            if (!read_status(reader, entry)) {
                return false;
            }
        }
        output.reportedCount = static_cast<std::uint8_t>(output.received.size());
    } else if (prefix == kStatusSingle) {
        std::uint64_t outOfOrder = 0;
        std::uint64_t index = 0;
        if (!reader.read(kFlagWidth, outOfOrder) || !reader.read(kSingleIndexWidth, index)) {
            return false;
        }
        // The named entry is the last one and the only one that is not an ordinary receive.
        output.received.fill(true);
        output.received[static_cast<std::size_t>(index)] = outOfOrder != 0;
        output.reportedCount = static_cast<std::uint8_t>(index + 1);
    } else {
        std::uint64_t ringState = 0;
        if (!reader.read(kFlagWidth, ringState)) {
            return false;
        }
    }
    std::uint64_t delay = 0;
    if (!reader.read(kDelayWidth, delay)) {
        return false;
    }
    output.delay = static_cast<std::uint16_t>(delay);
    output.ringInitialized = true;
    return true;
}

/**
 * Reads one reliable queue payload.
 * @param reader Open reader positioned at the queue payload.
 * @param fragmentBytes Fixed fragment size of this queue.
 * @param shortLengthWidth Width of this queue's short-fragment length field.
 * @param output Receives every record the packet carried.
 * @return True when the record list was well formed.
 */
[[nodiscard]] bool read_queue(bits::Reader& reader,
                              std::size_t fragmentBytes,
                              std::uint8_t shortLengthWidth,
                              QueueRecords& output) noexcept {
    output.count = 0;
    std::uint64_t recordsPresent = 0;
    if (!reader.read(kFlagWidth, recordsPresent)) {
        return false;
    }
    if (recordsPresent == 0) {
        return true;
    }
    std::uint16_t previous = 0;
    bool hasPrevious = false;
    for (;;) {
        std::uint64_t selector = 0;
        if (!reader.read(kSelectorWidth, selector)) {
            return false;
        }
        if (selector == kSelectorEnd) {
            return true;
        }
        if (output.count >= output.records.size()) {
            return false;
        }
        QueueRecord& record = output.records[output.count];
        record = {};
        if (selector == kSelectorAbsolute) {
            std::uint64_t sequence = 0;
            if (!reader.read(kAbsoluteSequenceWidth, sequence)) {
                return false;
            }
            record.sequence = static_cast<std::uint16_t>(sequence);
        } else if (selector == kSelectorDelta) {
            std::uint64_t delta = 0;
            if (!hasPrevious || !reader.read(kDeltaWidth, delta)) {
                return false;
            }
            record.sequence = static_cast<std::uint16_t>((previous + 1 + delta) % kSequenceModulus);
        } else {
            if (!hasPrevious) {
                return false;
            }
            record.sequence = static_cast<std::uint16_t>((previous + 1) % kSequenceModulus);
        }
        previous = record.sequence;
        hasPrevious = true;

        std::uint64_t isShort = 0;
        if (!reader.read(kFlagWidth, isShort)) {
            return false;
        }
        record.shortFragment = isShort != 0;
        if (record.shortFragment) {
            std::uint64_t secondary = 0;
            std::uint64_t lengthMinusOne = 0;
            if (!reader.read(kFlagWidth, secondary)
                || !reader.read(shortLengthWidth, lengthMinusOne)) {
                return false;
            }
            record.bitCount = static_cast<std::uint16_t>(lengthMinusOne + 1);
        } else {
            record.bitCount = static_cast<std::uint16_t>(fragmentBytes * kByteBits);
        }
        if (record.bitCount > record.bytes.size() * kByteBits) {
            return false;
        }
        // Fragment bits are read as whole bytes plus a remainder, so the tail keeps its padding.
        const std::size_t wholeBytes = record.bitCount / kByteBits;
        const std::uint8_t remainder = static_cast<std::uint8_t>(record.bitCount % kByteBits);
        if (!bits::read_raw(reader, std::span<std::byte>(record.bytes.data(), wholeBytes))) {
            return false;
        }
        if (remainder != 0) {
            std::uint64_t tail = 0;
            if (!reader.read(remainder, tail)) {
                return false;
            }
            record.bytes[wholeBytes] = static_cast<std::byte>(tail << (kByteBits - remainder));
        }
        ++output.count;
    }
}

} // namespace

/** Decodes one established packet up to and including both reliable queues. */
bool decode_established(std::span<const std::byte> payload,
                        bool expectExternal,
                        EstablishedPacket& output) noexcept {
    bits::Reader reader(payload);
    std::uint64_t marker = 0;
    std::uint64_t fragmented = 0;
    std::uint64_t guard = 0;
    if (!reader.read(kFlagWidth, marker) || marker != kEstablishedMarker
        || !reader.read(kFlagWidth, fragmented) || fragmented != 0
        || !reader.read(kSequenceGuardWidth, guard)) {
        return false;
    }
    output = {};
    output.connectionSequenceLow2 = static_cast<std::uint8_t>(guard);
    if (!read_ack(reader, output.ack)
        || !read_queue(reader, kLargeFragmentBytes, kLargeShortLengthWidth, output.large)
        || !read_queue(reader, kSmallFragmentBytes, kSmallShortLengthWidth, output.small)) {
        return false;
    }
    // The sentinel handler writes no bits, so the external body starts here when one exists.
    output.hasExternal = expectExternal;
    output.externalBitOffset = payload.size() * kByteBits - reader.remaining_bits();
    return true;
}

/** Reports whether one acknowledgement covers a packet this host sent. */
bool acknowledgement_covers(const AckState& ack, std::uint16_t sentSequence) noexcept {
    if (!ack.ringInitialized) {
        return false;
    }
    const auto base = static_cast<std::uint16_t>(ack.receiveHead % kPacketRingSize);
    const auto sent = static_cast<std::uint16_t>(sentSequence % kPacketRingSize);
    const auto distance = static_cast<std::uint16_t>((base - sent) % kPacketRingSize);
    if (distance >= kPacketRingSize / 2) {
        // The base is behind the packet, so the peer has not reached it yet.
        return false;
    }
    if (distance == 0) {
        // The base is the newest packet the peer holds, and it carries no status entry.
        return true;
    }
    if (distance <= ack.reportedCount) {
        return ack.received[distance - 1U];
    }
    // Older than every entry the peer named, so it has left the peer's window.
    return true;
}

/** Writes the packet head and the acknowledgement handler payload. */
bool write_head_and_ack(bits::Writer& writer,
                        std::uint8_t connectionSequenceLow2,
                        const AckState& ack) noexcept {
    if (!writer.write(kEstablishedMarker, kFlagWidth) || !writer.write(0, kFlagWidth)
        || !writer.write(connectionSequenceLow2, kSequenceGuardWidth)) {
        return false;
    }
    if (!writer.write(ack.outboundHeadPresent ? 1U : 0U, kFlagWidth)
        || !writer.write(ack.outboundHead, kOutboundHeadWidth)
        || !writer.write(ack.headMinusCursor, kCursorWidth)) {
        return false;
    }
    if (!ack.ringInitialized) {
        // Nothing has been received yet, so the uninitialized form ends the payload here.
        return writer.write(0, kAckBaseWidth) && writer.write(kStatusExtended, kStatusPrefixWidth)
               && writer.write(1, kStatusExtensionWidth);
    }
    if (!writer.write(ack.receiveHead, kAckBaseWidth)
        || !writer.write(kStatusExplicit, kStatusPrefixWidth)) {
        return false;
    }
    // Eight explicit statuses say exactly which packets arrived, without claiming any others.
    for (const bool received : ack.received) {
        if (!write_status(writer, received)) {
            return false;
        }
    }
    return writer.write(ack.delay, kDelayWidth);
}

/** Writes one reliable queue that carries no records. */
bool write_empty_queue(bits::Writer& writer) noexcept {
    return writer.write(0, kFlagWidth);
}

/** Writes one reliable queue and every fragment it owes. */
bool write_queue(bits::Writer& writer, const state::gameplay::OutboundQueue& queue) noexcept {
    if (queue.count == 0) {
        return write_empty_queue(writer);
    }
    if (!writer.write(1, kFlagWidth)) {
        return false;
    }
    for (std::size_t index = 0; index < queue.count; ++index) {
        const state::gameplay::OutboundFragment& fragment = queue.fragments[index];
        if (!writer.write(kSelectorAbsolute, kSelectorWidth)
            || !writer.write(fragment.sequence, kAbsoluteSequenceWidth)
            || !writer.write(fragment.shortFragment ? 1U : 0U, kFlagWidth)) {
            return false;
        }
        if (fragment.shortFragment) {
            // The secondary flag has no meaning for a fragment this host writes and stays clear.
            if (!writer.write(0, kFlagWidth)
                || !writer.write(fragment.bitCount - 1U, kLargeShortLengthWidth)) {
                return false;
            }
        }
        const std::size_t wholeBytes = fragment.bitCount / kByteBits;
        const auto remainder = static_cast<std::uint8_t>(fragment.bitCount % kByteBits);
        if (!bits::write_raw(writer, {fragment.bytes.data(), wholeBytes})) {
            return false;
        }
        if (remainder != 0) {
            const auto tail = std::to_integer<std::uint64_t>(fragment.bytes[wholeBytes]);
            if (!writer.write(tail >> (kByteBits - remainder), remainder)) {
                return false;
            }
        }
    }
    return writer.write(kSelectorEnd, kSelectorWidth);
}

/** Splits one reliable message into fragments and appends them to a send queue. */
bool enqueue_message(state::gameplay::OutboundQueue& queue,
                     std::uint8_t id,
                     std::uint32_t declaredSize,
                     std::span<const std::byte> body,
                     std::size_t bodyBits) noexcept {
    if (id > kMaximumMessageId) {
        return false;
    }
    // The inner header precedes the body, so the message is staged once and then split.
    std::array<std::byte, state::gameplay::kReassemblyCapacity> staged{};
    bits::Writer writer(staged);
    if (!writer.write(id, kMessageIdWidth) || !writer.write(declaredSize, kMessageSizeWidth)) {
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
    std::size_t stagedBytes = 0;
    if (!writer.finish(stagedBytes)) {
        return false;
    }

    const std::size_t totalBits = writer.bit_count();
    // Every fragment but the last carries the queue's whole fixed size.
    constexpr std::size_t kFragmentBits = kLargeFragmentBytes * kByteBits;
    std::size_t consumed = 0;
    bits::Reader source({staged.data(), stagedBytes});
    // A half-enqueued message can never be reassembled, so any failure below restores the queue.
    const std::size_t entryCount = queue.count;
    const std::uint16_t entrySequence = queue.nextSequence;
    const auto restore = [&queue, entryCount, entrySequence]() noexcept {
        for (std::size_t index = entryCount; index < queue.count; ++index) {
            queue.fragments[index] = {};
        }
        queue.count = entryCount;
        queue.nextSequence = entrySequence;
    };
    while (consumed < totalBits) {
        if (queue.count >= queue.fragments.size()) {
            restore();
            return false;
        }
        const std::size_t take =
            (totalBits - consumed) < kFragmentBits ? totalBits - consumed : kFragmentBits;
        // Only a short fragment closes a run. A full last fragment still needs one appended.
        const bool last = consumed + take >= totalBits;
        state::gameplay::OutboundFragment& fragment = queue.fragments[queue.count];
        fragment = {};
        bits::Writer chunk(fragment.bytes);
        std::size_t written = 0;
        std::size_t pending = take;
        while (pending != 0) {
            const auto width = static_cast<std::uint8_t>(pending < kByteBits ? pending : kByteBits);
            std::uint64_t value = 0;
            if (!source.read(width, value) || !chunk.write(value, width)) {
                restore();
                return false;
            }
            pending -= width;
        }
        if (!chunk.finish(written)) {
            restore();
            return false;
        }
        fragment.sequence = queue.nextSequence;
        fragment.bitCount = static_cast<std::uint16_t>(take);
        fragment.shortFragment = last && take < kFragmentBits;
        fragment.occupied = true;
        queue.nextSequence = static_cast<std::uint16_t>((queue.nextSequence + 1)
                                                        % state::gameplay::kMessageSequenceModulus);
        ++queue.count;
        consumed += take;
        if (last && !fragment.shortFragment) {
            // An exact multiple of the fragment size still needs a closing short fragment.
            if (queue.count >= queue.fragments.size()) {
                restore();
                return false;
            }
            state::gameplay::OutboundFragment& terminator = queue.fragments[queue.count];
            terminator = {};
            terminator.sequence = queue.nextSequence;
            terminator.bitCount = 1;
            terminator.shortFragment = true;
            terminator.occupied = true;
            queue.nextSequence = static_cast<std::uint16_t>(
                (queue.nextSequence + 1) % state::gameplay::kMessageSequenceModulus);
            ++queue.count;
        }
    }
    return true;
}

/** Reads the bounded outer filler and requires exact zero byte padding. */
bool read_filler_and_padding(bits::Reader& reader, FillerTrailer& output) noexcept {
    FillerTrailer candidate{};
    std::uint64_t present = 0;
    if (!reader.read(kFlagWidth, present)) {
        return false;
    }
    candidate.present = present != 0;
    if (candidate.present) {
        std::uint64_t bitCount = 0;
        if (!reader.read(kFillerLengthWidth, bitCount) || bitCount == 0
            || bitCount > kMaximumFillerBits || !reader.skip(static_cast<std::size_t>(bitCount))) {
            return false;
        }
        candidate.bitCount = static_cast<std::size_t>(bitCount);
    }

    const std::size_t paddingBits = reader.remaining_bits();
    std::uint64_t padding = 0;
    if (paddingBits > kMaximumPaddingBits
        || !reader.read(static_cast<std::uint8_t>(paddingBits), padding) || padding != 0) {
        return false;
    }
    output = candidate;
    return true;
}

/** Writes the filler trailer that ends every packet. */
bool write_absent_filler(bits::Writer& writer) noexcept {
    // Two bits close a packet: the extended-presence bit, then the external-body present bit.
    // The reader consumes both, so both must be written even though both are zero.
    return writer.write(0, kFlagWidth) && writer.write(0, kFlagWidth);
}

} // namespace sunrise::middleware::gameplay::peer
