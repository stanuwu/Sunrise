#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::encoding::bits {
class Reader;
}

namespace sunrise::middleware::bap::activity_message::telemetry {

/** The client asks the host to reserve membership rows for peers. */
inline constexpr std::uint32_t kReservationRequestType = 13;
/** A debug command. It is retained and never executed. */
inline constexpr std::uint32_t kDebugCommandType = 34;
/** A client heartbeat carrying one runtime-selected nested body. */
inline constexpr std::uint32_t kHeartbeatType = 39;
/** An opaque scalar report whose meaning is not known. */
inline constexpr std::uint32_t kBugClawType = 43;
/** A periodic batch of peer packet-gap counters. */
inline constexpr std::uint32_t kLagSwitchType = 46;
/** A periodic connection-quality header followed by peer records. */
inline constexpr std::uint32_t kConnectionQualityType = 47;
/** A fixed telemetry block sent at most once a minute. */
inline constexpr std::uint32_t kHighWaterType = 49;
/** An opaque scalar request whose known wrapper always sends logical zero. */
inline constexpr std::uint32_t kRefreshInspirationsType = 50;

/** Peer-bearing telemetry arrays have 32 records in the native definitions. */
inline constexpr std::size_t kPeerRecordCapacity = 32;
/** Machine identity fields carry eight struct-order bytes. */
inline constexpr std::size_t kPeerKeySize = 8;
/** Message 13 begins with a 32-bit epoch and a six-bit record count. */
inline constexpr std::size_t kReservationBaseBits = 38;
/** Each message-13 reservation record is 362 bits. */
inline constexpr std::size_t kReservationRecordBits = 362;
/** The complete message-13 maximum is 11,622 meaningful bits. */
inline constexpr std::size_t kReservationMaximumBits = 11622;
/** An opaque biased scalar body is one 32-bit word. */
inline constexpr std::size_t kScalarSize = 4;
/** The high-water body is exactly 416 bits. */
inline constexpr std::size_t kHighWaterSize = 52;
/** The lag-switch record count is six bits, so the wire can declare more than is supported. */
inline constexpr std::uint8_t kLagRecordCountWidth = 6;
/** Each message-46 peer record is 256 bits. */
inline constexpr std::size_t kLagRecordBits = 256;
/** The complete message-46 maximum is 8,198 meaningful bits. */
inline constexpr std::size_t kLagMaximumBits = 8198;
/** Message 47 always begins with a 352-bit aggregate header and a six-bit count. */
inline constexpr std::size_t kConnectionQualityBaseBits = 358;
/** Each message-47 peer record is 353 bits. */
inline constexpr std::size_t kConnectionQualityRecordBits = 353;
/** The complete message-47 maximum is 11,654 meaningful bits. */
inline constexpr std::size_t kConnectionQualityMaximumBits = 11654;
/** The high-water body opens with nine unsigned 32-bit words. */
inline constexpr std::size_t kHighWaterNarrowCount = 9;
/** Two unsigned 64-bit words close the high-water body. */
inline constexpr std::size_t kHighWaterWideCount = 2;

/** One identity requested for a future membership row. */
struct ReservationRecord {
    std::uint64_t machineKey{};
    std::int32_t memberIndex{};
    /** The sender writes -1. No reader has named this field. */
    std::int32_t field2{};
    std::uint64_t playerKey{};
    std::int64_t accountSoid{};
    std::int64_t characterSoid{};
    std::uint64_t groupMemberQword{};
};

/** Shared native BC record reader; startup collections carry adjacent, unpadded records. */
[[nodiscard]] bool read_reservation_identity(encoding::bits::Reader& reader,
                                             ReservationRecord& record) noexcept;

/** One complete peer-reservation request. */
struct ReservationRequest {
    std::uint32_t peerTableEpoch{};
    std::uint8_t recordCount{};
    std::array<ReservationRecord, kPeerRecordCapacity> records{};
};

/** One peer's six packet-gap values in message 46. */
struct LagSwitchRecord {
    std::uint64_t peerKey{};
    /** Four counters retain their native nested wire order. */
    std::array<std::int32_t, 4> gapCounts{};
    std::int32_t longestReceptionGap{};
    std::int32_t longestAckGap{};
};

/** One complete lag-switch report. */
struct LagSwitchReport {
    std::uint8_t recordCount{};
    std::array<LagSwitchRecord, kPeerRecordCapacity> records{};
};

/** The fixed aggregate header at the start of message 47. */
struct ConnectionQualityHeader {
    std::int32_t minimumChannels{};
    std::int32_t maximumChannels{};
    std::int32_t maximumBitsPerSecond{};
    std::int32_t averageCongestionBitsPerSecond{};
    std::int32_t lowCongestionBitsPerSecond{};
    std::int32_t highCongestionBitsPerSecond{};
    std::array<std::int32_t, 3> latency{};
    /** Raw IEEE-754 bits preserve both packet-loss fields exactly. */
    std::array<std::uint32_t, 2> packetLossBits{};
};

/** One peer row in message 47. */
struct ConnectionQualityRecord {
    std::uint64_t peerKey{};
    /** The native log calls this `bh`; its meaning is unresolved. */
    bool property{};
    std::array<std::int32_t, 2> longestGaps{};
    std::int32_t sendsPerSecond{};
    std::int32_t sendBitsPerSecond{};
    std::int32_t receivesPerSecond{};
    std::int32_t receiveBitsPerSecond{};
    std::int32_t roundTripLatency{};
    /** Raw IEEE-754 bits preserve the per-peer packet-loss field. */
    std::uint32_t packetLossBits{};
    std::int32_t discardCount{};
};

/** One complete connection-quality report. */
struct ConnectionQualityReport {
    ConnectionQualityHeader header{};
    std::uint8_t recordCount{};
    std::array<ConnectionQualityRecord, kPeerRecordCapacity> records{};
};

/** One high-water telemetry block. Every leaf meaning is opaque. */
struct HighWater {
    std::array<std::uint32_t, kHighWaterNarrowCount> narrow{};
    std::array<std::uint64_t, kHighWaterWideCount> wide{};
};

/**
 * Parses a complete peer-reservation request.
 * @param input Activity payload after the envelope.
 * @param request Cleared first, then filled.
 * @param consumedBits Receives the bits the revision used.
 * @return True only for the exact count-selected body and zero byte padding.
 */
[[nodiscard]] bool parse_reservation_request(std::span<const std::byte> input,
                                             ReservationRequest& request,
                                             std::size_t& consumedBits) noexcept;

/**
 * Parses a complete lag-switch report.
 * @param input Activity payload after the envelope.
 * @param report Cleared first, then filled.
 * @param consumedBits Receives the bits the count used.
 * @return True only for the exact count-selected body and zero byte padding.
 */
[[nodiscard]] bool parse_lag_switch(std::span<const std::byte> input,
                                    LagSwitchReport& report,
                                    std::size_t& consumedBits) noexcept;

/**
 * Parses a complete connection-quality report.
 * @param input Activity payload after the envelope.
 * @param report Cleared first, then filled.
 * @param consumedBits Receives the meaningful schema bits on success.
 * @return True only for the exact count-selected body and zero byte padding.
 */
[[nodiscard]] bool parse_connection_quality(std::span<const std::byte> input,
                                            ConnectionQualityReport& report,
                                            std::size_t& consumedBits) noexcept;

/**
 * Parses the fixed high-water telemetry block.
 * @param input Activity payload after the envelope.
 * @param block Cleared first, then filled.
 * @param consumedBits Receives the bits the block used.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool parse_high_water(std::span<const std::byte> input,
                                    HighWater& block,
                                    std::size_t& consumedBits) noexcept;

/**
 * Parses one biased signed scalar body, shared by the two opaque scalar messages.
 * @param input Activity payload after the envelope.
 * @param value Receives the decoded scalar.
 * @param consumedBits Receives the bits the scalar used.
 * @return True when the whole fixed body was present.
 */
[[nodiscard]] bool parse_opaque_scalar(std::span<const std::byte> input,
                                       std::int32_t& value,
                                       std::size_t& consumedBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::telemetry
