/**
 * Telemetry and reservation bodies. Each one is framed only as far as its recovered grammar
 * reaches, and the caller is told how many bits that was, so a body that is retained rather than
 * understood is never counted as fully read.
 */

#include <bit>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "telemetry.h"

namespace sunrise::middleware::bap::activity_message::telemetry {
namespace {

/** Opaque scalar bodies are biased by the midpoint of the unsigned 32-bit range. */
constexpr std::uint32_t kScalarBias = 0x80000000U;
/** Every high-water word is read most significant bit first, as the rest of the body is. */
constexpr std::uint8_t kNarrowWidth = 32;
/** See kNarrowWidth. */
constexpr std::uint8_t kWideWidth = 64;

} // namespace

/** Parses a peer-reservation request. */
bool parse_reservation_request(std::span<const std::byte> input,
                               ReservationRequest& request,
                               std::size_t& consumedBits) noexcept {
    request = {};
    consumedBits = 0;
    if (input.size() < kReservationRevisionSize) {
        return false;
    }
    request.revision = encoding::read_u32_be(input.first<encoding::kU32Size>());
    request.recordBytes = static_cast<std::uint32_t>(input.size() - kReservationRevisionSize);
    consumedBits = kReservationRevisionSize * encoding::kBitsPerByte;
    return true;
}

/** Parses a lag-switch report as far as its record count. */
bool parse_lag_switch(std::span<const std::byte> input,
                      LagSwitchReport& report,
                      std::size_t& consumedBits) noexcept {
    report = {};
    consumedBits = 0;
    encoding::bits::Reader reader(input);
    std::uint64_t count = 0;
    if (!reader.read(kLagRecordCountWidth, count)) {
        return false;
    }
    report.recordCount = static_cast<std::uint8_t>(count);
    report.aboveSupported = report.recordCount > kLagRecordSupported;
    report.recordBits = static_cast<std::uint32_t>(reader.remaining_bits());
    consumedBits = kLagRecordCountWidth;
    return true;
}

/** Parses the fixed high-water telemetry block. */
bool parse_high_water(std::span<const std::byte> input,
                      HighWater& block,
                      std::size_t& consumedBits) noexcept {
    block = {};
    consumedBits = 0;
    if (input.size() < kHighWaterSize) {
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
    if (input.size() < kScalarSize) {
        return false;
    }
    const std::uint32_t raw = encoding::read_u32_be(input.first<encoding::kU32Size>());
    value = std::bit_cast<std::int32_t>(raw - kScalarBias);
    consumedBits = kScalarSize * encoding::kBitsPerByte;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::telemetry
