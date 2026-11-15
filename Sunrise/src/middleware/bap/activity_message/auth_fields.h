#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::auth_fields {

/** One fixed-width wire field. The row that builds it states what the value asserts. */
struct Field final {
    std::uint64_t value{};
    std::uint8_t width{};
};

/** Presence bit that precedes every optional schema field. */
inline constexpr std::uint8_t kPresenceWidth = 1;
/** A schema boolean is one bit. */
inline constexpr std::uint8_t kBoolWidth = 1;
/** Signed 32-bit schema fields store zero at the middle of the unsigned wire range. */
inline constexpr std::uint32_t kSigned32Bias = 0x80000000U;
/** Generations and revisions are 31-bit counters that the client accepts only while positive. */
inline constexpr std::uint8_t kCounterWidth = 31;
inline constexpr std::uint32_t kMaximumCounter = 0x7FFFFFFFU;
/** A ClientRef is a 32-bit registry key, a 7-bit biased slot type and a 16-bit biased index. */
inline constexpr std::uint8_t kClientRefKeyWidth = 32;
inline constexpr std::uint8_t kClientRefTypeWidth = 7;
inline constexpr std::uint8_t kClientRefIndexWidth = 16;
inline constexpr std::uint32_t kClientRefTypeBias = 1;
inline constexpr std::uint32_t kClientRefIndexBias = 32'768;
inline constexpr std::uint16_t kMaximumClientRefIndex = 32'767;
/** The unset ClientRef carries the no-name hash, type zero and index -1. */
inline constexpr std::uint32_t kClientRefAbsentKey = 0x811C9DC5U;
inline constexpr std::size_t kClientRefBits =
    kClientRefKeyWidth + kClientRefTypeWidth + kClientRefIndexWidth;
/** Native durations count 673,200 ticks per second. */
inline constexpr std::uint64_t kTicksPerSecond = 673'200;

/** Writes the fields in order. @return False when the writer runs out of room. */
[[nodiscard]] inline bool write_fields(encoding::bits::Writer& writer,
                                       std::span<const Field> fields) noexcept {
    for (const Field& field : fields) {
        if (!writer.write(field.value, field.width)) {
            return false;
        }
    }
    return true;
}

/** Writes one present ClientRef naming an exact slot. */
[[nodiscard]] inline bool write_client_ref(encoding::bits::Writer& writer,
                                           std::uint32_t registryKey,
                                           std::uint32_t slotType,
                                           std::uint16_t slotIndex) noexcept {
    return writer.write(registryKey, kClientRefKeyWidth)
           && writer.write(slotType + kClientRefTypeBias, kClientRefTypeWidth)
           && writer.write(slotIndex + kClientRefIndexBias, kClientRefIndexWidth);
}

/** Writes the unset ClientRef. */
[[nodiscard]] inline bool write_absent_client_ref(encoding::bits::Writer& writer) noexcept {
    return writer.write(kClientRefAbsentKey, kClientRefKeyWidth)
           && writer.write(0, kClientRefTypeWidth)
           && writer.write(kClientRefIndexBias - 1U, kClientRefIndexWidth);
}

/** @return True when the writer holds exactly the expected bits and closes into the byte count. */
[[nodiscard]] inline bool finish_exact(encoding::bits::Writer& writer,
                                       std::size_t bits,
                                       std::size_t bytes,
                                       std::size_t& written) noexcept {
    return writer.bit_count() == bits && writer.finish(written) && written == bytes;
}

/** @return True when a body's byte count is the one its bit count needs. */
[[nodiscard]] constexpr bool bytes_match_bits(std::size_t bytes, std::size_t bits) noexcept {
    return bytes == (bits + 7U) / 8U;
}

} // namespace sunrise::middleware::bap::activity_message::auth_fields
