/** Complete structural decoders for bounded telemetry and reservation bodies. */

#include <bit>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "telemetry.h"

namespace sunrise::middleware::bap::activity_message::telemetry {
namespace {

/** Opaque scalar bodies are biased by the midpoint of the unsigned 32-bit range. */
constexpr std::uint32_t kScalarBias = 0x80000000U;
/** Reservation member indices use a one-step signed bias. */
constexpr std::uint32_t kMemberIndexBias = 1;
/** All dynamic peer counts use six wire bits. */
constexpr std::uint8_t kPeerCountWidth = 6;
/** Reservation member indices use ten wire bits. */
constexpr std::uint8_t kMemberIndexWidth = 10;
/** Every high-water word is read most significant bit first, as the rest of the body is. */
constexpr std::uint8_t kNarrowWidth = 32;
/** See kNarrowWidth. */
constexpr std::uint8_t kWideWidth = 64;

/** @return Complete bytes needed for the given meaningful bit count. */
[[nodiscard]] constexpr std::size_t bytes_for_bits(std::size_t bits) noexcept {
    return (bits + encoding::kBitsPerByte - 1) / encoding::kBitsPerByte;
}

/** Reads one struct-order eight-byte identity into its little-endian scalar form. */
[[nodiscard]] bool read_peer_key(encoding::bits::Reader& reader, std::uint64_t& value) noexcept {
    value = 0;
    for (std::size_t index = 0; index < kPeerKeySize; ++index) {
        std::uint64_t byte = 0;
        if (!reader.read(encoding::kBitsPerByte, byte)) {
            value = 0;
            return false;
        }
        value |= byte << (index * encoding::kBitsPerByte);
    }
    return true;
}

/** Reads one midpoint-biased signed 32-bit field. */
[[nodiscard]] bool read_biased_i32(encoding::bits::Reader& reader, std::int32_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(kNarrowWidth, raw)) {
        value = 0;
        return false;
    }
    value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw) - kScalarBias);
    return true;
}

/** Accepts only the zero padding that completes the current byte. */
[[nodiscard]] bool finish_padding(encoding::bits::Reader& reader) noexcept {
    const std::size_t paddingBits = reader.remaining_bits();
    if (paddingBits == 0) {
        return true;
    }
    std::uint64_t padding = 0;
    return paddingBits < encoding::kBitsPerByte
           && reader.read(static_cast<std::uint8_t>(paddingBits), padding) && padding == 0
           && reader.remaining_bits() == 0;
}

/** Reads one complete message-13 reservation row. */
[[nodiscard]] bool read_reservation_record(encoding::bits::Reader& reader,
                                           ReservationRecord& record) noexcept {
    std::uint64_t raw = 0;
    std::uint64_t account = 0;
    std::uint64_t character = 0;
    if (!read_peer_key(reader, record.machineKey) || !reader.read(kMemberIndexWidth, raw)) {
        return false;
    }
    record.memberIndex =
        static_cast<std::int32_t>(raw) - static_cast<std::int32_t>(kMemberIndexBias);
    if (!read_biased_i32(reader, record.field2) || !read_peer_key(reader, record.playerKey)
        || !reader.read(kWideWidth, account) || !reader.read(kWideWidth, character)
        || !reader.read(kWideWidth, record.groupMemberQword)) {
        return false;
    }
    record.accountSoid = std::bit_cast<std::int64_t>(account);
    record.characterSoid = std::bit_cast<std::int64_t>(character);
    return true;
}

/** Reads one complete message-46 peer row. */
[[nodiscard]] bool read_lag_record(encoding::bits::Reader& reader,
                                   LagSwitchRecord& record) noexcept {
    if (!read_peer_key(reader, record.peerKey)) {
        return false;
    }
    for (std::int32_t& count : record.gapCounts) {
        if (!read_biased_i32(reader, count)) {
            return false;
        }
    }
    return read_biased_i32(reader, record.longestReceptionGap)
           && read_biased_i32(reader, record.longestAckGap);
}

/** Reads the fixed aggregate header at the start of message 47. */
[[nodiscard]] bool read_connection_header(encoding::bits::Reader& reader,
                                          ConnectionQualityHeader& header) noexcept {
    std::array<std::int32_t*, 6> bandwidthFields{
        &header.minimumChannels,
        &header.maximumChannels,
        &header.maximumBitsPerSecond,
        &header.averageCongestionBitsPerSecond,
        &header.lowCongestionBitsPerSecond,
        &header.highCongestionBitsPerSecond,
    };
    for (std::int32_t* field : bandwidthFields) {
        if (!read_biased_i32(reader, *field)) {
            return false;
        }
    }
    for (std::int32_t& latency : header.latency) {
        if (!read_biased_i32(reader, latency)) {
            return false;
        }
    }
    std::uint64_t raw = 0;
    for (std::uint32_t& loss : header.packetLossBits) {
        if (!reader.read(kNarrowWidth, raw)) {
            return false;
        }
        loss = static_cast<std::uint32_t>(raw);
    }
    return true;
}

/** Reads one complete message-47 peer row. */
[[nodiscard]] bool read_connection_record(encoding::bits::Reader& reader,
                                          ConnectionQualityRecord& record) noexcept {
    std::uint64_t field = 0;
    if (!read_peer_key(reader, record.peerKey) || !reader.read(1, field)) {
        return false;
    }
    record.property = field != 0;
    for (std::int32_t& gap : record.longestGaps) {
        if (!read_biased_i32(reader, gap)) {
            return false;
        }
    }
    std::array<std::int32_t*, 5> rateFields{
        &record.sendsPerSecond,
        &record.sendBitsPerSecond,
        &record.receivesPerSecond,
        &record.receiveBitsPerSecond,
        &record.roundTripLatency,
    };
    for (std::int32_t* rate : rateFields) {
        if (!read_biased_i32(reader, *rate)) {
            return false;
        }
    }
    if (!reader.read(kNarrowWidth, field)) {
        return false;
    }
    record.packetLossBits = static_cast<std::uint32_t>(field);
    return read_biased_i32(reader, record.discardCount);
}

} // namespace

bool read_reservation_identity(encoding::bits::Reader& reader, ReservationRecord& record) noexcept {
    record = {};
    ReservationRecord parsed{};
    if (!read_reservation_record(reader, parsed)) {
        return false;
    }
    record = parsed;
    return true;
}

/** Parses a complete peer-reservation request. */
bool parse_reservation_request(std::span<const std::byte> input,
                               ReservationRequest& request,
                               std::size_t& consumedBits) noexcept {
    request = {};
    consumedBits = 0;
    encoding::bits::Reader reader(input);
    ReservationRequest parsed{};
    std::uint64_t field = 0;
    if (!reader.read(kNarrowWidth, field)) {
        return false;
    }
    parsed.peerTableEpoch = static_cast<std::uint32_t>(field);
    if (!reader.read(kPeerCountWidth, field) || field > kPeerRecordCapacity) {
        return false;
    }
    parsed.recordCount = static_cast<std::uint8_t>(field);
    const std::size_t bodyBits = kReservationBaseBits + kReservationRecordBits * parsed.recordCount;
    if (bodyBits > kReservationMaximumBits || input.size() != bytes_for_bits(bodyBits)) {
        return false;
    }
    for (std::size_t index = 0; index < parsed.recordCount; ++index) {
        if (!read_reservation_record(reader, parsed.records[index])) {
            return false;
        }
    }
    if (!finish_padding(reader)) {
        return false;
    }
    request = parsed;
    consumedBits = bodyBits;
    return true;
}

/** Parses a complete lag-switch report. */
bool parse_lag_switch(std::span<const std::byte> input,
                      LagSwitchReport& report,
                      std::size_t& consumedBits) noexcept {
    report = {};
    consumedBits = 0;
    encoding::bits::Reader reader(input);
    LagSwitchReport parsed{};
    std::uint64_t count = 0;
    if (!reader.read(kLagRecordCountWidth, count) || count > kPeerRecordCapacity) {
        return false;
    }
    parsed.recordCount = static_cast<std::uint8_t>(count);
    const std::size_t bodyBits = kLagRecordCountWidth + kLagRecordBits * parsed.recordCount;
    if (bodyBits > kLagMaximumBits || input.size() != bytes_for_bits(bodyBits)) {
        return false;
    }
    for (std::size_t index = 0; index < parsed.recordCount; ++index) {
        if (!read_lag_record(reader, parsed.records[index])) {
            return false;
        }
    }
    if (!finish_padding(reader)) {
        return false;
    }
    report = parsed;
    consumedBits = bodyBits;
    return true;
}

/** Parses a complete connection-quality report. */
bool parse_connection_quality(std::span<const std::byte> input,
                              ConnectionQualityReport& report,
                              std::size_t& consumedBits) noexcept {
    report = {};
    consumedBits = 0;
    encoding::bits::Reader reader(input);
    ConnectionQualityReport parsed{};
    std::uint64_t count = 0;
    if (!read_connection_header(reader, parsed.header) || !reader.read(kPeerCountWidth, count)
        || count > kPeerRecordCapacity) {
        return false;
    }
    parsed.recordCount = static_cast<std::uint8_t>(count);
    const std::size_t bodyBits =
        kConnectionQualityBaseBits + kConnectionQualityRecordBits * parsed.recordCount;
    if (bodyBits > kConnectionQualityMaximumBits || input.size() != bytes_for_bits(bodyBits)) {
        return false;
    }
    for (std::size_t index = 0; index < parsed.recordCount; ++index) {
        if (!read_connection_record(reader, parsed.records[index])) {
            return false;
        }
    }
    if (!finish_padding(reader)) {
        return false;
    }
    report = parsed;
    consumedBits = bodyBits;
    return true;
}

/** Parses the fixed high-water telemetry block. */
bool parse_high_water(std::span<const std::byte> input,
                      HighWater& block,
                      std::size_t& consumedBits) noexcept {
    block = {};
    consumedBits = 0;
    if (input.size() != kHighWaterSize) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t field = 0;
    for (std::uint32_t& word : block.narrow) {
        if (!reader.read(kNarrowWidth, field)) {
            return false;
        }
        word = static_cast<std::uint32_t>(field);
    }
    for (std::uint64_t& word : block.wide) {
        if (!reader.read(kWideWidth, field)) {
            return false;
        }
        word = field;
    }
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    return true;
}

/** Parses one biased signed scalar body. */
bool parse_opaque_scalar(std::span<const std::byte> input,
                         std::int32_t& value,
                         std::size_t& consumedBits) noexcept {
    value = 0;
    consumedBits = 0;
    if (input.size() != kScalarSize) {
        return false;
    }
    const std::uint32_t raw = encoding::read_u32_be(input.first<encoding::kU32Size>());
    value = std::bit_cast<std::int32_t>(raw - kScalarBias);
    consumedBits = kScalarSize * encoding::kBitsPerByte;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::telemetry
