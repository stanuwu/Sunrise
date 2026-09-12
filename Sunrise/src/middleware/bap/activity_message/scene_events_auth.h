#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "sensor_auth_update.h"

// Type-43 authored scene Auth in its event-only form: generation, no clear, no dependencies,
// a zero scalar, then the cumulative event keys. The generation stays while events are added,
// because a new generation restarts the scene.

namespace sunrise::middleware::bap::activity_message::scene_events {

namespace fields = auth_fields;

inline constexpr std::uint8_t kSlotType = 43;
/** The SDK format table carries the same class and schema for authored scenes. */
inline constexpr std::uint32_t kComponentClass = 0x80806382U;
inline constexpr std::uint32_t kSchema = 0x8080626BU;
/** Header: 32-bit generation, clear bit, 4-bit dependency count, 31-bit scalar, 6-bit count. */
inline constexpr std::uint8_t kDependencyCountWidth = 4;
inline constexpr std::uint8_t kScalarWidth = 31;
inline constexpr std::uint8_t kEventCountWidth = 6;
inline constexpr std::size_t kHeaderBits =
    32 + fields::kBoolWidth + kDependencyCountWidth + kScalarWidth + kEventCountWidth;
inline constexpr std::uint8_t kEventKeyWidth = 32;
/** Events this API carries per body; the 6-bit count could name more. */
inline constexpr std::size_t kMaximumEvents = 32;
inline constexpr std::size_t kMaximumBytes =
    (kHeaderBits + kEventKeyWidth * kMaximumEvents + 7) / 8;
/** All-one bits is not an event key. */
inline constexpr std::uint32_t kInvalidEventKey = 0xFFFFFFFFU;

/**
 * Encodes the scene body.
 * @param generation Positive scene generation.
 * @param events Distinct nonzero event keys.
 * @param bytes Receives the byte count. @param bits Receives the meaningful bit count.
 */
[[nodiscard]] inline bool encode(std::int32_t generation,
                                 std::span<const std::uint32_t> events,
                                 std::span<std::byte> output,
                                 std::size_t& bytes,
                                 std::size_t& bits) noexcept {
    bytes = 0;
    bits = 0;
    return generation > 0
           && sensor_auth_update::encode_authored_scene_auth(
               static_cast<std::uint32_t>(generation), {}, output, bytes, bits, events);
}

} // namespace sunrise::middleware::bap::activity_message::scene_events
