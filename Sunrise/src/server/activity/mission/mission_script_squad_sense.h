#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808094.h"

namespace sunrise::server::activity::mission {

namespace generated = middleware::content::packages::schema::generated;

/** Type-1 squad Sense nested schemas: the lost-count list, its counts, and the cost reals. */
inline constexpr std::uint32_t kSquadLostListSchema = generated::C80807ecf::kClass;
inline constexpr std::uint32_t kSquadLostCountSchema = generated::C80809491::kClass;
inline constexpr std::uint32_t kSquadObjectiveCostSchema = generated::C80807ecd::kClass;
/** Root ordinals: field 1 echoes the objective revision, field 3 is the alive count. */
inline constexpr std::uint16_t kSquadObjectiveRevisionOrdinal = 1;
inline constexpr std::uint16_t kSquadAliveOrdinal = 3;
/** The lost-count list has at most eight slots. */
inline constexpr std::size_t kSquadLostSlotCapacity = 8;
/** One cost per authored objective task group. */
inline constexpr std::size_t kSquadObjectiveGroupCount = 24;
/** Costs are saturated distances; the client never publishes more than this. */
inline constexpr float kMaximumObjectiveCost = 2040.0F;
/** The native schema initializes every cost to its unreachable value. */
inline constexpr std::uint32_t kSquadObjectiveDefaultMask =
    (std::uint32_t{1} << kSquadObjectiveGroupCount) - 1U;
/** The alive count is six bits on the wire. */
inline constexpr std::int64_t kMaximumAliveCount = 63;
inline constexpr std::int64_t kMaximumCounter = 0x7FFFFFFF;

/**
 * Reads the per-slot lost counts. The client raises a slot's count as it tears down one of that
 * slot's actors, and a new spawn generation clears them, so a rise after the first sighting is a
 * loss. The squad Sense does not report creations.
 * @param output Receives one count per slot; slots past the list length read zero.
 * @return The list length, or zero when the list is absent or incomplete.
 */
[[nodiscard]] inline std::uint8_t read_squad_lost_counts(
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::span<std::int32_t> output) noexcept {
    std::uint32_t length = 0;
    std::uint32_t known = 0;
    std::array<std::int32_t, kSquadLostSlotCapacity> counts{};
    for (const auto& value : values) {
        if (!value.present) {
            continue;
        }
        if (value.schemaRow == kSquadLostListSchema && value.fieldOrdinal == 0) {
            if (value.unsignedValue > counts.size()) {
                return 0;
            }
            length = static_cast<std::uint32_t>(value.unsignedValue);
        } else if (value.schemaRow == kSquadLostCountSchema && value.fieldOrdinal == 0
                   && value.occurrence < counts.size()) {
            if (value.signedValue < 0 || value.signedValue > kMaximumCounter) {
                return 0;
            }
            counts[value.occurrence] = static_cast<std::int32_t>(value.signedValue);
            known |= 1U << value.occurrence;
        }
    }
    if (length == 0 || length > output.size() || known != (1U << length) - 1) {
        return 0;
    }
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = index < length ? counts[index] : 0;
    }
    return static_cast<std::uint8_t>(length);
}

/** Native defaults remain until a present cost delta replaces them. */
struct SquadObjectiveCosts final {
    std::array<float, kSquadObjectiveGroupCount> values = [] {
        std::array<float, kSquadObjectiveGroupCount> defaults{};
        defaults.fill(kMaximumObjectiveCost);
        return defaults;
    }();
    std::uint32_t known{kSquadObjectiveDefaultMask};
    std::uint32_t revision{};
    bool operator==(const SquadObjectiveCosts&) const = default;
};

/**
 * Merges present cost deltas over the retained native defaults.
 * @param retained Current costs for this squad and source generation.
 * @param values Accepted squad Sense fields.
 * @param root Native schema identity.
 * @return True when a cost or revision changed.
 */
[[nodiscard]] inline bool update_squad_objective_costs(
    SquadObjectiveCosts& retained,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    bool changed = false;
    for (const auto& value : values) {
        if (!value.present) {
            continue;
        }
        if (value.schemaRow == root && value.fieldOrdinal == kSquadObjectiveRevisionOrdinal
            && value.signedValue >= 0 && value.signedValue <= kMaximumCounter) {
            const auto revision = static_cast<std::uint32_t>(value.signedValue);
            changed = changed || retained.revision != revision;
            retained.revision = revision;
        } else if (value.schemaRow == kSquadObjectiveCostSchema && value.fieldOrdinal == 0
                   && value.occurrence < kSquadObjectiveGroupCount && std::isfinite(value.realValue)
                   && value.realValue >= 0 && value.realValue <= kMaximumObjectiveCost) {
            const std::uint32_t bit = 1U << value.occurrence;
            changed = changed || (retained.known & bit) == 0
                      || retained.values[value.occurrence] != value.realValue;
            retained.known |= bit;
            retained.values[value.occurrence] = value.realValue;
        }
    }
    return changed;
}

/**
 * Reads the root alive count. An optional field still yields a row when absent, and only a
 * present count is evidence of a population change; zero is a real count.
 */
[[nodiscard]] inline bool read_squad_alive(
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root,
    std::int32_t& alive) noexcept {
    for (const auto& value : values) {
        if (value.schemaRow == root && value.fieldOrdinal == kSquadAliveOrdinal && value.present) {
            if (value.signedValue < 0 || value.signedValue > kMaximumAliveCount) {
                return false;
            }
            alive = static_cast<std::int32_t>(value.signedValue);
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::activity::mission
