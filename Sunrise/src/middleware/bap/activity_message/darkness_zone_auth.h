#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808099.h"
#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

// Type-35 hard-wipe globals sensor. Field .0 enables the darkness restriction; the timer block
// selects the native wipe countdown, published in whole seconds with the elapsed value clamped.

namespace sunrise::middleware::bap::activity_message::darkness_zone {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Type 35 uses this schema and fixed body size.
inline constexpr std::uint8_t kSlotType = 35;
inline constexpr std::uint32_t kComponentClass = generated::C808099bd::kClass;
inline constexpr std::uint32_t kSchema = generated::C808099bf::kClass;
inline constexpr std::size_t kBits = 359;
inline constexpr std::size_t kBytes = 45;
/** Native durations count 673,200 ticks per second. */
inline constexpr std::uint64_t kTicksPerSecond = 673'200;
/** The wipe countdown runs three seconds. */
inline constexpr int kMaximumWipeSeconds = 3;
/** No countdown requested. */
inline constexpr int kNoWipe = -1;
/** Field .1 and .2 are written as 0 and 1. Meaning unverified. */
inline constexpr std::uint8_t kSecondFieldWidth = 1;
inline constexpr std::uint8_t kThirdFieldWidth = 2;
inline constexpr std::uint32_t kThirdFieldValue = 1;
/** Field .3 selects the countdown: 1 with bias one selects the native wipe timer. */
inline constexpr std::uint8_t kTimerSelectWidth = 2;
/** Five 64-bit tick words: elapsed three times, remaining, then zero. */
inline constexpr std::size_t kTimerWordCount = 5;
inline constexpr std::uint8_t kTimerWordWidth = 64;
/** The trailing 32-bit scale is IEEE 1.0 while a countdown runs, else zero. */
inline constexpr std::uint32_t kOneAsFloatBits = 0x3F800000U;

/**
 * Encodes the sensor body.
 * @param enabled Field .0, the darkness restriction.
 * @param wipeSeconds Remaining countdown seconds, or kNoWipe. A countdown needs enabled.
 * @param output Exactly kBytes.
 */
[[nodiscard]] inline bool
encode(bool enabled, std::span<std::byte> output, int wipeSeconds = kNoWipe) noexcept {
    if (output.size() != kBytes || wipeSeconds < kNoWipe || wipeSeconds > kMaximumWipeSeconds
        || (!enabled && wipeSeconds != kNoWipe)) {
        return false;
    }
    const bool wipe = wipeSeconds != kNoWipe;
    const std::uint64_t elapsed =
        wipe ? static_cast<std::uint64_t>(kMaximumWipeSeconds - wipeSeconds) * kTicksPerSecond : 0;
    const std::uint64_t remaining =
        wipe ? static_cast<std::uint64_t>(wipeSeconds) * kTicksPerSecond : 0;
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 5> header{{
        {enabled ? 1U : 0U, fields::kBoolWidth},
        {0, kSecondFieldWidth},
        {kThirdFieldValue, kThirdFieldWidth},
        {wipe ? 1U : 0U, kTimerSelectWidth},
        {wipe ? 1U : 0U, fields::kBoolWidth},
    }};
    const std::array<fields::Field, kTimerWordCount> timer{{
        {elapsed, kTimerWordWidth},
        {elapsed, kTimerWordWidth},
        {elapsed, kTimerWordWidth},
        {remaining, kTimerWordWidth},
        {0, kTimerWordWidth},
    }};
    return fields::write_fields(writer, header) && fields::write_fields(writer, timer)
           && writer.write(wipe ? kOneAsFloatBits : 0U, 32)
           && fields::finish_exact(writer, kBits, kBytes, written);
}

/**
 * Reads field .0 back from a body this tree encoded, for the roster's darkness policy.
 * @return False when the body is not this sensor's size.
 */
[[nodiscard]] inline bool
read_enabled(std::span<const std::byte> body, std::size_t bits, bool& enabled) noexcept {
    if (bits != kBits || body.size() != kBytes) {
        return false;
    }
    encoding::bits::Reader reader(body);
    std::uint64_t value = 0;
    if (!reader.read(fields::kBoolWidth, value)) {
        return false;
    }
    enabled = value != 0;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::darkness_zone
