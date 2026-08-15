#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::high_water {

/** Activity high-water reports use activity message type 49. */
inline constexpr std::uint32_t kMessageType = 49;
/** The opaque high-water body is exactly 52 wire bytes. */
inline constexpr std::size_t kEncodedSize = 52;

/**
 * Checks one opaque high-water body without reading any payload byte.
 * @param input Complete type-49 payload with no trailing bytes.
 * @return True when the payload has the exact declared wire size.
 */
[[nodiscard]] bool validate_high_water(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::activity_message::high_water
