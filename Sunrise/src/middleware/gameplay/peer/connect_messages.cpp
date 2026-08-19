#include "connect_messages.h"

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::peer {

namespace {

namespace bits = encoding::bits;

/** Both sequence fields are 32-bit value fields. */
constexpr std::uint8_t kSequenceWidth = 32;
/** The refuse reason is three bits. */
constexpr std::uint8_t kRefuseReasonWidth = 3;
/** The close reason is five bits. */
constexpr std::uint8_t kCloseReasonWidth = 5;
/** The ping sequence is a 16-bit value field. */
constexpr std::uint8_t kPingSequenceWidth = 16;
/** The ping timestamp is a 64-bit value field. */
constexpr std::uint8_t kPingTimestampWidth = 64;
/** The ping's trailing flag and the pong's response kind are the only fields that differ. */
constexpr std::uint8_t kPingFlagWidth = 1;
/** The pong response kind is two bits. */
constexpr std::uint8_t kPongKindWidth = 2;
/** The discarded-packet count is five bits. */
constexpr std::uint8_t kDiscardedWidth = 5;
/** A mayday names its group session in raw bits. */
constexpr std::uint8_t kMaydaySessionWidth = 64;
/** The mayday code is nine bits and decodes to one above its wire value. */
constexpr std::uint8_t kMaydayCodeWidth = 9;
/** The mayday code carries a bias of one. */
constexpr std::uint16_t kMaydayCodeBias = 1;

/** Reads the two sequence fields every connection message starts with. */
[[nodiscard]] bool
read_sequences(bits::Reader& reader, std::uint32_t& connection, std::uint32_t& transport) noexcept {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    if (!reader.read(kSequenceWidth, first) || !reader.read(kSequenceWidth, second)) {
        return false;
    }
    connection = static_cast<std::uint32_t>(first);
    transport = static_cast<std::uint32_t>(second);
    return true;
}

/** Writes the two sequence fields every connection message starts with. */
[[nodiscard]] bool
write_sequences(bits::Writer& writer, std::uint32_t connection, std::uint32_t transport) noexcept {
    return writer.write(connection, kSequenceWidth) && writer.write(transport, kSequenceWidth);
}

} // namespace

/** Reads a connect request body. */
bool read_request(bits::Reader& reader, ConnectRequest& output) noexcept {
    return read_sequences(reader, output.channelId, output.sequence)
           && bits::read_raw(reader, output.address);
}

/** Writes a connect response body. */
bool write_response(bits::Writer& writer, const ConnectResponse& body) noexcept {
    // The echoed pair must lead. The requester closes the connection when it does not match.
    return write_sequences(writer, body.remoteChannelId, body.remoteSequence)
           && write_sequences(writer, body.channelId, body.sequence)
           && bits::write_raw(writer, body.address);
}

/** Reads a connect establish body. */
bool read_establish(bits::Reader& reader, ConnectEstablish& output) noexcept {
    return read_sequences(reader, output.remoteChannelId, output.channelId);
}

/** Writes a connect establish body. */
bool write_establish(bits::Writer& writer, const ConnectEstablish& body) noexcept {
    return write_sequences(writer, body.remoteChannelId, body.channelId);
}

/** Writes a connect refuse body. */
bool write_refuse(bits::Writer& writer, const ConnectEnd& body) noexcept {
    return write_sequences(writer, body.remoteChannelId, body.remoteSequence)
           && writer.write(body.reason, kRefuseReasonWidth);
}

/** Writes a connect closed body. */
bool write_closed(bits::Writer& writer, const ConnectEnd& body) noexcept {
    return write_sequences(writer, body.remoteChannelId, body.remoteSequence)
           && writer.write(body.reason, kCloseReasonWidth);
}

/** Reads a connect closed body. */
bool read_closed(bits::Reader& reader, ConnectEnd& output) noexcept {
    std::uint64_t reason = 0;
    if (!read_sequences(reader, output.remoteChannelId, output.remoteSequence)
        || !reader.read(kCloseReasonWidth, reason)) {
        return false;
    }
    output.reason = static_cast<std::uint8_t>(reason);
    return true;
}

/** Reads a ping body. */
bool read_ping(bits::Reader& reader, PingBody& output) noexcept {
    std::uint64_t sequence = 0;
    std::uint64_t flag = 0;
    if (!reader.read(kPingSequenceWidth, sequence)
        || !reader.read(kPingTimestampWidth, output.timestamp)
        || !reader.read(kPingFlagWidth, flag)) {
        return false;
    }
    output.sequence = static_cast<std::uint16_t>(sequence);
    output.flag = flag != 0;
    return true;
}

/** Writes a pong body. */
bool write_pong(bits::Writer& writer, const PongBody& body) noexcept {
    // The sender matches a pong to its ping on the echoed pair, so both lead unaltered.
    return writer.write(body.sequence, kPingSequenceWidth)
           && writer.write(body.timestamp, kPingTimestampWidth)
           && writer.write(body.responseKind, kPongKindWidth);
}

/** Reads a packets-discarded diagnostic. */
bool read_packets_discarded(bits::Reader& reader, std::uint8_t& discarded) noexcept {
    std::uint64_t count = 0;
    if (!reader.read(kDiscardedWidth, count)) {
        return false;
    }
    discarded = static_cast<std::uint8_t>(count);
    return true;
}

/** Reads a recovery diagnostic. */
bool read_mayday(bits::Reader& reader, MaydayBody& output) noexcept {
    MaydayBody candidate{};
    std::uint64_t code = 0;
    if (!reader.read(kMaydaySessionWidth, candidate.sessionId)
        || !reader.read(kMaydayCodeWidth, code)) {
        return false;
    }
    candidate.code = static_cast<std::uint16_t>(code) + kMaydayCodeBias;
    if (candidate.code > kMaydayCodeMaximum) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::gameplay::peer
