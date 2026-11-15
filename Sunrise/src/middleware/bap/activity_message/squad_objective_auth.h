#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"
#include "squad_auth_body.h"

// Objective updates preserve placement fields and may refresh player awareness once per revision.
// One may also replace the squad's temperament list, which keeps the placement's spawn
// generation, so living members merge the new list instead of being respawned.

namespace sunrise::middleware::bap::activity_message::squad_objective {

namespace fields = auth_fields;

// Squad objective updates use this Type 1 schema and fixed prefix size.
inline constexpr std::uint32_t kSchema = squad_auth::kSchema;
inline constexpr std::size_t kBits = 153;
inline constexpr std::size_t kBytes = 20;
/** A present `.2` replaces its absent presence bit with the list. */
[[nodiscard]] constexpr std::size_t
temperament_extra_bits(const std::optional<squad_auth::Temperaments>& temperaments) noexcept {
    return temperaments.has_value() ? squad_auth::temperament_bit_count(temperaments->count) : 0;
}
/** Optional root .14 adds one 31-bit awareness revision; a full `.2` adds its six ids. */
inline constexpr std::size_t kMaximumBits =
    kBits + fields::kCounterWidth
    + squad_auth::temperament_bit_count(squad_auth::kTemperamentCapacity);
inline constexpr std::size_t kMaximumBytes = (kMaximumBits + 7U) / 8U;
/** Absent presence bits for field .1, before the temperament list. */
inline constexpr std::uint8_t kAbsentBeforeTemperamentsCount = 1;
/** Absent presence bits for fields .3 to .12, after the temperament list. */
inline constexpr std::uint8_t kAbsentAfterTemperamentsCount = 10;
/** Root .15 carries the biased -1 sentinel; its role is unverified. */
inline constexpr std::uint8_t kField15Width = 6;
/** Root .16 task group: 5 bits with bias one, so -1 requests costs without a link. */
inline constexpr std::uint8_t kTaskGroupWidth = 5;
inline constexpr std::int32_t kTaskGroupBias = 1;
inline constexpr std::int32_t kNoTaskGroup = -1;
/** One objective sensor carries this many task groups. */
inline constexpr std::int32_t kTaskGroupCount = 24;
/**
 * Root .18 active, 2 bits, written as the active wire value. Active and mode have no presence
 * bit; over a transported placement, compose keeps the placement's values (see patch_fields).
 */
inline constexpr std::uint8_t kActiveWidth = 2;
inline constexpr std::uint32_t kActiveValue = 2;
/** Root .19 mode, 3 bits with bias one. Reserve keeps the counts for a delivery. */
inline constexpr std::uint8_t kModeWidth = 3;
inline constexpr std::uint32_t kModeBias = 1;

struct Request final {
    std::uint32_t registryKey{};
    std::uint32_t revision{};
    std::uint16_t objectiveIndex{};
    std::int32_t taskGroup{kNoTaskGroup};
    bool reserved{};
    bool refreshPlayerAwareness{};
    /** Sent as `.2`, empty to clear it; absent keeps the list the squad last received. */
    std::optional<squad_auth::Temperaments> temperaments{};
};

[[nodiscard]] constexpr std::size_t bit_count(const Request& request) noexcept {
    return kBits + (request.refreshPlayerAwareness ? fields::kCounterWidth : 0)
           + temperament_extra_bits(request.temperaments);
}

[[nodiscard]] constexpr std::size_t byte_count(const Request& request) noexcept {
    return (bit_count(request) + 7U) / 8U;
}

/**
 * Encodes the objective assignment.
 * @param output Exactly byte_count(request) bytes.
 * @return False on an out-of-range revision, index or task group.
 */
[[nodiscard]] inline bool encode(const Request& request, std::span<std::byte> output) noexcept {
    if (output.size() != byte_count(request) || request.registryKey == 0 || request.revision == 0
        || request.revision > fields::kMaximumCounter
        || request.objectiveIndex > fields::kMaximumClientRefIndex
        || request.taskGroup < kNoTaskGroup || request.taskGroup >= kTaskGroupCount
        || (request.temperaments.has_value()
            && !squad_auth::valid_temperaments(*request.temperaments))) {
        return false;
    }
    const auto mode = static_cast<std::uint32_t>(request.reserved ? squad_auth::Mode::reserve
                                                                  : squad_auth::Mode::mode2);
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 9> tail{{
        {1, fields::kPresenceWidth}, // .15 present
        {0, kField15Width},
        {1, fields::kPresenceWidth}, // .16 present
        {static_cast<std::uint32_t>(request.taskGroup + kTaskGroupBias), kTaskGroupWidth},
        {0, fields::kPresenceWidth}, // .17 absent
        {kActiveValue, kActiveWidth},
        {mode + kModeBias, kModeWidth},
        {1, fields::kPresenceWidth},       // .20 present
        {fields::kClientRefAbsentKey, 32}, // no name
    }};
    return writer.write(1, fields::kPresenceWidth)
           && fields::write_client_ref(
               writer, request.registryKey, scriptable_auth::kType3SlotType, request.objectiveIndex)
           && writer.write(0, kAbsentBeforeTemperamentsCount)
           && squad_auth::write_temperaments(writer, request.temperaments)
           && writer.write(0, kAbsentAfterTemperamentsCount)
           && writer.write(1, fields::kPresenceWidth)
           && writer.write(request.revision, fields::kCounterWidth)
           && writer.write(request.refreshPlayerAwareness, fields::kPresenceWidth)
           && (!request.refreshPlayerAwareness
               || writer.write(request.revision, fields::kCounterWidth))
           && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, bit_count(request), byte_count(request), written);
}

} // namespace sunrise::middleware::bap::activity_message::squad_objective
