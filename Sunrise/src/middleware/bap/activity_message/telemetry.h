#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::telemetry {

/** The client asks for peer reservations. The record array behind the revision is unresolved. */
inline constexpr std::uint32_t kReservationRequestType = 13;
/** A debug command. It is retained and never executed. */
inline constexpr std::uint32_t kDebugCommandType = 34;
/** A client heartbeat carrying one runtime-selected nested body. */
inline constexpr std::uint32_t kHeartbeatType = 39;
/** An opaque scalar report whose meaning is not recovered. */
inline constexpr std::uint32_t kBugClawType = 43;
/** A lag-switch report: one record count and a record array of unresolved shape. */
inline constexpr std::uint32_t kLagSwitchType = 46;
/** A connection-quality report holding two nested structures of unresolved leaf shape. */
inline constexpr std::uint32_t kConnectionQualityType = 47;
/** A fixed telemetry block sent at most once a minute. */
inline constexpr std::uint32_t kHighWaterType = 49;
/** An opaque scalar request whose known wrapper always sends logical zero. */
inline constexpr std::uint32_t kRefreshInspirationsType = 50;

/** The reservation revision is one unsigned 32-bit word ahead of the record array. */
inline constexpr std::size_t kReservationRevisionSize = 4;
/** An opaque biased scalar body is one 32-bit word. */
inline constexpr std::size_t kScalarSize = 4;
/** The high-water body is exactly 416 bits. */
inline constexpr std::size_t kHighWaterSize = 52;
/** The lag-switch record count is six bits, so the wire can declare more than is supported. */
inline constexpr std::uint8_t kLagRecordCountWidth = 6;
/** Records above this count have no backing evidence, so they are retained and not acted on. */
inline constexpr std::uint8_t kLagRecordSupported = 32;
/** The high-water body opens with nine unsigned 32-bit words. */
inline constexpr std::size_t kHighWaterNarrowCount = 9;
/** Two unsigned 64-bit words close the high-water body. */
inline constexpr std::size_t kHighWaterWideCount = 2;

/** One peer-reservation request. The records behind the revision are kept as a bounded region. */
struct ReservationRequest {
    std::uint32_t revision{};
    /** Bytes of record data after the revision. Their grammar is unresolved. */
    std::uint32_t recordBytes{};
};

/** One lag-switch report. The records behind the count have an unresolved grammar. */
struct LagSwitchReport {
    std::uint8_t recordCount{};
    /** Bits after the count. Their grammar is unresolved. */
    std::uint32_t recordBits{};
    /** Set when the declared count is above what the record grammar is known to support. */
    bool aboveSupported{};
};

/** One high-water telemetry block. Every leaf meaning is opaque. */
struct HighWater {
    std::array<std::uint32_t, kHighWaterNarrowCount> narrow{};
    std::array<std::uint64_t, kHighWaterWideCount> wide{};
};

/**
 * Parses a peer-reservation request.
 * @param input Activity payload after the envelope.
 * @param request Cleared first, then filled.
 * @param consumedBits Receives the bits the revision used.
 * @return True when the revision was present.
 */
[[nodiscard]] bool parse_reservation_request(std::span<const std::byte> input,
                                             ReservationRequest& request,
                                             std::size_t& consumedBits) noexcept;

/**
 * Parses a lag-switch report as far as its record count.
 * @param input Activity payload after the envelope.
 * @param report Cleared first, then filled.
 * @param consumedBits Receives the bits the count used.
 * @return True when the count was present.
 */
[[nodiscard]] bool parse_lag_switch(std::span<const std::byte> input,
                                    LagSwitchReport& report,
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
