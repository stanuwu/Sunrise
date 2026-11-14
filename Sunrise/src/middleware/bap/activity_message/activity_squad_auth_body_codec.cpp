#include <algorithm>
#include <array>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "definition.h"
#include "scriptable_auth_internal.h"
#include "squad_auth_body.h"

namespace sunrise::middleware::bap::activity_message::squad_auth {
namespace {

namespace bits = encoding::bits;

/** Requested counts use the signed 32-bit midpoint as their wire bias. */
constexpr std::uint32_t kRequestedCountBias = 0x80000000U;
/** Every optional root field has one presence bit. */
constexpr std::uint8_t kPresenceWidth = 1;
/** The requested-count array length occupies the full four-bit count lane. */
constexpr std::uint8_t kRequestedCountLengthWidth = 4;
/** Each requested member count is one biased signed 32-bit value. */
constexpr std::uint8_t kRequestedCountWidth = static_cast<std::uint8_t>(kRequestedCountBitCount);
/** Nested field 5 contains bias-one widths 3, 2, 3, 2, 3. */
constexpr std::array<std::uint8_t, 5> kAuthoredProfileWidths{3, 2, 3, 2, 3};
constexpr std::uint32_t kAuthoredProfileBias = 1;
/** Spawn generation is the schema's unsigned 31-bit value. */
constexpr std::uint8_t kGenerationWidth = 31;
/** Active and mode are bias-one fields with widths two and three. */
constexpr std::uint8_t kActiveWidth = 2;
constexpr std::uint8_t kModeWidth = 3;
/** Mode is a bias-one signed field. */
constexpr std::uint32_t kModeBias = 1;
/** A present name hash is one exact unsigned 32-bit value. */
constexpr std::uint8_t kNameHashWidth = 32;
/** Logical active one becomes wire two after the schema bias; logical zero becomes one. */
constexpr std::uint32_t kActiveWireValue = 2;
constexpr std::uint32_t kInactiveWireValue = 1;
/** Fields zero and one precede the temperament list. */
constexpr std::size_t kLeadingAbsentFieldCount = 2;
/** One unused dynamic field separates requested counts from generation. */
constexpr std::size_t kAfterCountsAbsentFieldCount = 1;
/**
 * Eleven dynamic fields separate generation from active, `.7` to `.17`. `.11` and `.12` are
 * the spawn references the point-set resolver reads, and it keeps the package's own authored
 * rule only for the unset ClientRef. `.10` and `.17` carry the destination when one is set.
 */
constexpr std::size_t kBeforeDestinationAbsentFieldCount = 3;
constexpr std::size_t kAfterSpawnReferenceAbsentFieldCount = 4;
/** The destination request revision is a 31-bit counter the client echoes at +600. */
constexpr std::uint8_t kDestinationRevisionWidth = 31;
/** A spawn rule is a type-66 slot. */
constexpr std::uint8_t kSpawnRuleSlotType = 66;

/** @return True when the guard itself can name a prior positive generation. */
[[nodiscard]] constexpr bool valid_guard(const GenerationGuard& guard) noexcept {
    return !guard.hasLast || (guard.last > 0 && guard.last <= kMaximumGeneration);
}

/** @return True when every caller-controlled field is in its exact schema range. */
[[nodiscard]] bool valid(const Preset& preset, const GenerationGuard& guard) noexcept {
    if (preset.requestedCounts.size() < kMinimumRequestedCountLength
        || preset.requestedCounts.size() > kMaximumRequestedCountLength || !valid_guard(guard)
        || preset.generation == 0 || preset.generation > kMaximumGeneration
        || (guard.hasLast && preset.generation <= guard.last)) {
        return false;
    }
    if (!valid_mode(preset.mode)) {
        return false;
    }
    if (!std::ranges::all_of(preset.requestedCounts,
                             [](std::int32_t count) { return count >= 0; })) {
        return false;
    }
    for (std::size_t index = 0; index < preset.authoredProfile.size(); ++index) {
        const std::int32_t value = preset.authoredProfile[index];
        const std::uint8_t width = kAuthoredProfileWidths[index + 1U];
        if (value < 0 || value >= (std::int32_t{1} << width) - 1) {
            return false;
        }
    }
    if (preset.destination.has_value()
        && (preset.destination->registryKey == 0
            || preset.destination->slotIndex > auth_fields::kMaximumClientRefIndex)) {
        return false;
    }
    if (preset.spawnRule.has_value()
        && (preset.spawnRule->registryKey == 0
            || preset.spawnRule->slotIndex > auth_fields::kMaximumClientRefIndex)) {
        return false;
    }
    return !preset.temperaments.has_value() || valid_temperaments(*preset.temperaments);
}

/** @return Exact number of meaningful schema bits for one validated preset. */
[[nodiscard]] constexpr std::size_t body_bit_count(const Preset& preset) noexcept {
    return exact_body_bit_count(preset.requestedCounts.size(),
                                preset.destination.has_value(),
                                preset.temperaments.has_value()
                                    ? std::optional<std::size_t>{preset.temperaments->count}
                                    : std::nullopt);
}

/** Writes a repeated absent-field presence marker. */
[[nodiscard]] bool write_absent(bits::Writer& writer, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (!writer.write(0, kPresenceWidth)) {
            return false;
        }
    }
    return true;
}

/** Writes the root fields in schema order and leaves every unrelated optional field absent. */
[[nodiscard]] bool write_body(bits::Writer& writer, const Preset& preset) noexcept {
    if (!write_absent(writer, kLeadingAbsentFieldCount)
        || !write_temperaments(writer, preset.temperaments) || !writer.write(1, kPresenceWidth)
        || !writer.write(preset.requestedCounts.size(), kRequestedCountLengthWidth)) {
        return false;
    }
    for (const std::int32_t count : preset.requestedCounts) {
        const std::uint32_t encoded = static_cast<std::uint32_t>(count) + kRequestedCountBias;
        if (!writer.write(encoded, kRequestedCountWidth)) {
            return false;
        }
    }
    if (!write_absent(writer, kAfterCountsAbsentFieldCount) || !writer.write(1, kPresenceWidth)
        || !writer.write(kAuthoredProfileBias, kAuthoredProfileWidths[0])) {
        return false;
    }
    for (std::size_t index = 0; index < preset.authoredProfile.size(); ++index) {
        if (!writer.write(static_cast<std::uint32_t>(preset.authoredProfile[index])
                              + kAuthoredProfileBias,
                          kAuthoredProfileWidths[index + 1U])) {
            return false;
        }
    }
    const std::uint8_t mode = static_cast<std::uint8_t>(preset.mode);
    if (!writer.write(1, kPresenceWidth) || !writer.write(preset.generation, kGenerationWidth)
        || !write_absent(writer, kBeforeDestinationAbsentFieldCount)) {
        return false;
    }
    if (preset.destination.has_value()) {
        if (!writer.write(1, kPresenceWidth)
            || !auth_fields::write_client_ref(writer,
                                              preset.destination->registryKey,
                                              kSlotType,
                                              preset.destination->slotIndex)) {
            return false;
        }
    } else if (!write_absent(writer, 1)) {
        return false;
    }
    for (std::size_t index = 0; index < kSpawnReferenceCount; ++index) {
        if (!writer.write(1, kPresenceWidth)) {
            return false;
        }
        // The point resolver reads both references and falls back to the package rule for an
        // unset one, so a set rule fills both.
        if (preset.spawnRule.has_value()) {
            if (!auth_fields::write_client_ref(writer,
                                               preset.spawnRule->registryKey,
                                               kSpawnRuleSlotType,
                                               preset.spawnRule->slotIndex)) {
                return false;
            }
        } else if (!scriptable_auth::write_absent_client_ref(writer)) {
            return false;
        }
    }
    // The client binds the destination only when this revision differs from its echo.
    if (!write_absent(writer, kAfterSpawnReferenceAbsentFieldCount)) {
        return false;
    }
    if (preset.destination.has_value()) {
        if (!writer.write(1, kPresenceWidth)
            || !writer.write(preset.generation, kDestinationRevisionWidth)) {
            return false;
        }
    } else if (!write_absent(writer, 1)) {
        return false;
    }
    // A name the host does not own is the no-name value, never zero: `sub_7FF7421A9720`
    // compares this field against it and walks the member collection when it differs.
    // Active reads inactive when nothing is requested, so the client destroys the owned actors
    // instead of only killing them.
    const bool hasRequests = std::any_of(preset.requestedCounts.begin(),
                                         preset.requestedCounts.end(),
                                         [](std::int32_t count) { return count > 0; });
    if (!writer.write(hasRequests ? kActiveWireValue : kInactiveWireValue, kActiveWidth)
        || !writer.write(static_cast<std::uint32_t>(mode) + kModeBias, kModeWidth)
        || !writer.write(1, kPresenceWidth)
        || !writer.write(preset.nameHash.value_or(kEmptyNameHash), kNameHashWidth)) {
        return false;
    }
    return writer.bit_count() == body_bit_count(preset);
}

} // namespace

/** Finds the next positive 31-bit spawn generation without wrapping. */
bool next_generation(const GenerationGuard& guard, std::uint32_t& next) noexcept {
    next = 0;
    if (!valid_guard(guard)) {
        return false;
    }
    if (!guard.hasLast) {
        next = 1;
        return true;
    }
    if (guard.last == kMaximumGeneration) {
        return false;
    }
    next = guard.last + 1;
    return true;
}

/** Encodes one canonical slot-type-1 body through a staging buffer. */
bool encode(const Preset& preset,
            const GenerationGuard& guard,
            std::span<std::byte> output,
            std::size_t& writtenBytes,
            std::size_t& writtenBits) noexcept {
    writtenBytes = 0;
    writtenBits = 0;
    if (!valid(preset, guard)) {
        return false;
    }
    const std::size_t expectedBits = body_bit_count(preset);
    const std::size_t expectedBytes = (expectedBits + 7) / 8;
    if (output.size() < expectedBytes) {
        return false;
    }

    std::array<std::byte, kMaximumByteCount> staged{};
    bits::Writer writer{std::span(staged).first(expectedBytes)};
    std::size_t stagedBytes = 0;
    if (!write_body(writer, preset) || !writer.finish(stagedBytes)
        || stagedBytes != expectedBytes) {
        return false;
    }
    std::ranges::copy(std::span(staged).first(stagedBytes), output.begin());
    writtenBytes = stagedBytes;
    writtenBits = expectedBits;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::squad_auth
