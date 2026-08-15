#include "status_fields.h"

#include <limits>

namespace sunrise::middleware::web_service::status {
namespace {

/** 5-bit status fields store logical success 0 as 1. */
constexpr std::uint8_t kStatusWidth = 5;
/** Descriptor bias added to the logical status code. */
constexpr std::int32_t kStatusBias = 1;
/** 5-bit status fields keep only their descriptor width. */
constexpr std::uint32_t kStatusMask = (1U << kStatusWidth) - 1U;
/** Status-pair values fill one signed 32-bit field. */
constexpr std::uint8_t kStatusValueWidth = 32;
/** Required boolean fields are 1 bit after their status pair. */
constexpr std::uint8_t kRequiredBoolWidth = 1;

/**
 * Writes one biased status field shared by every response shape.
 * @param writer Fixed-buffer MSB-first payload writer.
 * @param code Logical signed status code.
 * @return True when the 5-bit field fits.
 */
[[nodiscard]] bool write_code(encoding::bits::Writer& writer, std::int32_t code) noexcept {
    const auto biased =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(code) + kStatusBias) & kStatusMask;
    return writer.write(biased, kStatusWidth);
}

/**
 * Writes the signed status value using its minimum-int descriptor bias.
 * @param writer Fixed-buffer MSB-first payload writer.
 * @param value Logical signed status value.
 * @return True when the whole 32-bit field fits.
 */
[[nodiscard]] bool write_value(encoding::bits::Writer& writer, std::int32_t value) noexcept {
    const std::int64_t biased =
        static_cast<std::int64_t>(value) - (std::numeric_limits<std::int32_t>::min)();
    return writer.write(static_cast<std::uint32_t>(biased), kStatusValueWidth);
}

} // namespace

/** Writes the chosen status descriptor, keeping field order. */
bool write_fields(encoding::bits::Writer& writer,
                  ResponseShape shape,
                  const StatusResponse& response) noexcept {
    if (shape == ResponseShape::generic) {
        // Generic replies carry no status field; only the envelope trailer follows.
        return true;
    }
    bool encoded = write_code(writer, response.code);
    if (shape != ResponseShape::statusOnly) {
        encoded = encoded && write_value(writer, response.value);
    }
    if (shape == ResponseShape::statusPairWithBool) {
        encoded = encoded && writer.write(response.trailingBool ? 1U : 0U, kRequiredBoolWidth);
    }
    return encoded;
}

} // namespace sunrise::middleware::web_service::status
