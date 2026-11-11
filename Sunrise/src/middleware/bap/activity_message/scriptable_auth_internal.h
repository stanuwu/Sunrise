#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"

// Constants and primitives shared by the scriptable-auth codecs.
// Not a public interface: only those translation units include it.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/** Widths of the scalar wire fields, in bits, and the value a present flag carries. */
inline constexpr std::uint8_t kReal32Width = 32;
inline constexpr std::uint8_t kEnabled = 1;
inline constexpr std::uint8_t kSigned32Width = 32;
/** Signed 16-bit schema fields store zero at the middle of the unsigned wire range. */
inline constexpr std::uint32_t kSigned16Bias = 0x8000;
/** Wide schema integers travel as 64-bit fields. */
inline constexpr std::uint8_t kWideIntegerWidth = 64;
/** Two-bit mode scalars carry a bias of one, so -1 is the lowest value they can store. */
inline constexpr std::uint8_t kModeWidth = 2;
inline constexpr std::int8_t kMinimumMode = -1;
inline constexpr std::int8_t kMaximumMode = 2;
inline constexpr std::uint32_t kModeBias = 1;
using auth_fields::kBoolWidth;
using auth_fields::kClientRefAbsentKey;
using auth_fields::kClientRefIndexBias;
using auth_fields::kClientRefIndexWidth;
using auth_fields::kClientRefTypeWidth;
using auth_fields::kSigned32Bias;
using auth_fields::write_absent_client_ref;

/** @return True when the unused low bits in the final byte are zero. */
[[nodiscard]] inline bool finish_padding(encoding::bits::Reader& reader) noexcept {
    const std::size_t paddingBits = reader.remaining_bits();
    std::uint64_t padding = 0;
    return paddingBits < 8U && reader.read(static_cast<std::uint8_t>(paddingBits), padding)
           && padding == 0 && reader.remaining_bits() == 0;
}

/** Reads and requires an exact nested unset ClientRef. */
[[nodiscard]] inline bool read_absent_client_ref(encoding::bits::Reader& reader) noexcept {
    std::uint64_t key = 0;
    std::uint64_t type = 0;
    std::uint64_t index = 0;
    return reader.read(kSigned32Width, key) && reader.read(kClientRefTypeWidth, type)
           && reader.read(kClientRefIndexWidth, index) && key == kClientRefAbsentKey && type == 0
           && index == kClientRefIndexBias - 1U;
}

/** Writes the complete fixed-width 0x808099C4 child layout. */
[[nodiscard]] inline bool write_shared_timed_state(encoding::bits::Writer& writer,
                                                   const SharedTimedState& state) noexcept {
    return writer.write(state.running ? 1U : 0U, kBoolWidth)
           && writer.write(state.minimum, kWideIntegerWidth)
           && writer.write(state.maximum, kWideIntegerWidth)
           && writer.write(state.currentAtEpoch, kWideIntegerWidth)
           && writer.write(state.remainingAtEpoch, kWideIntegerWidth)
           && writer.write(state.epoch, kWideIntegerWidth)
           && writer.write(std::bit_cast<std::uint32_t>(state.rate), kReal32Width);
}

/** @return True when a two-bit bias-one scalar is representable without wrapping. */
[[nodiscard]] constexpr bool valid_mode(std::int8_t mode) noexcept {
    return mode >= kMinimumMode && mode <= kMaximumMode;
}

/**
 * Advances a signed generation that the client accepts only while it stays positive.
 * @param hasLast Whether a generation was already committed.
 * @param last Last committed generation.
 * @param next Set to zero first. Receives the next generation on success.
 * @return True when the next generation is positive and did not wrap.
 */
[[nodiscard]] inline bool
next_positive_generation(bool hasLast, std::int32_t last, std::int32_t& next) noexcept {
    next = 0;
    if (hasLast && (last < 0 || last == (std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    next = hasLast ? last + 1 : 1;
    return next > 0;
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
