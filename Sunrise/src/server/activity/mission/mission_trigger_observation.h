#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"
#include "../../../state/activity_sdk/format.h"

namespace sunrise::server::activity::mission::trigger_observation {

namespace sense = middleware::bap::activity_message::sense_update;
/** Type-30 occupancy Sense: two required flags, then two required signed 32-bit values. */
constexpr std::uint32_t kSchema = state::activity_sdk::format::kOccupancySenseSchema;
constexpr std::size_t kValueCount = 4;
/** Root ordinals: any member inside, all inside, intersection count, copied Auth value. */
constexpr std::uint16_t kOccupiedOrdinal = 0;
constexpr std::uint16_t kAllOrdinal = 1;
constexpr std::uint16_t kCountOrdinal = 2;
constexpr std::uint16_t kThresholdOrdinal = 3;
/** The two flags are one bit wide and the two numbers 32. */
constexpr std::uint8_t kFlagWidth = 1;
constexpr std::uint8_t kNumberWidth = 32;

/** One complete occupancy report. */
struct Snapshot final {
    bool occupied{};
    bool all{};
    std::int32_t count{};
    std::int32_t threshold{};
    bool operator==(const Snapshot&) const = default;
};

/** Reads one occupancy body. An incomplete or ambiguous body is refused, never defaulted. */
[[nodiscard]] inline bool read(std::span<const sense::DecodedValue> body,
                               Snapshot& output) noexcept {
    if (body.size() != kValueCount) {
        return false;
    }
    std::array<const sense::DecodedValue*, kValueCount> fields{};
    for (const auto& value : body) {
        if (value.schemaRow != kSchema || value.fieldOrdinal >= fields.size()
            || value.occurrence != 0 || !value.present || fields[value.fieldOrdinal]) {
            return false;
        }
        if (value.fieldOrdinal < kCountOrdinal) {
            if (value.kind != sense::ValueKind::boolean || value.width != kFlagWidth
                || value.unsignedValue > 1) {
                return false;
            }
        } else if (value.kind != sense::ValueKind::signedInteger || value.width != kNumberWidth
                   || value.signedValue < (std::numeric_limits<std::int32_t>::min)()
                   || value.signedValue > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        fields[value.fieldOrdinal] = &value;
    }
    output = {fields[kOccupiedOrdinal]->unsignedValue != 0,
              fields[kAllOrdinal]->unsignedValue != 0,
              static_cast<std::int32_t>(fields[kCountOrdinal]->signedValue),
              static_cast<std::int32_t>(fields[kThresholdOrdinal]->signedValue)};
    return true;
}

/** How one report relates to the last accepted one, as scripts see it. */
enum class Continuity : std::uint8_t {
    invalid,
    baseline,
    consecutive,
    duplicate,
    conflict,
    gap,
    reset,
    missingCounter,
    wrap
};
/** @return The stable script-facing name of one continuity value. */
[[nodiscard]] inline const char* name(Continuity value) noexcept {
    switch (value) {
    case Continuity::baseline:
        return "baseline";
    case Continuity::consecutive:
        return "consecutive";
    case Continuity::duplicate:
        return "duplicate";
    case Continuity::conflict:
        return "conflict";
    case Continuity::gap:
        return "gap";
    case Continuity::reset:
        return "reset";
    case Continuity::missingCounter:
        return "missing_counter";
    case Continuity::wrap:
        return "wrap";
    default:
        return "invalid";
    }
}
/** What one report means to scripts: its continuity, and whether they hear of it. */
struct Observation final {
    Continuity continuity{Continuity::invalid};
    /** Scripts receive a triggerState event for this report. */
    bool notify{};
    /** The report carried a complete level the event may expose. */
    bool available{};
};

/**
 * Classifies one volume's reports by their record counter. It never decides an enter or exit edge;
 * a repeated counter never confirms a level, and a conflicting repeat invalidates it.
 */
struct Tracker final {
    std::uint64_t source{}, sequence{};
    std::uint32_t counter{};
    bool hasCounter{}, known{};
    Snapshot snapshot{};

    /**
     * Classifies one report against the last accepted one.
     * @param current The decoded level, or null when the body was malformed.
     * @return Nothing to notify for a stale or foreign report or an exact duplicate.
     */
    [[nodiscard]] Observation observe(const Snapshot* current,
                                      std::uint64_t expectedSource,
                                      std::uint64_t nextSource,
                                      std::uint64_t nextSequence,
                                      std::uint32_t nextCounter,
                                      bool nextHasCounter) noexcept {
        if (nextSource == 0 || nextSource != expectedSource || nextSequence == 0
            || (source == nextSource && nextSequence <= sequence)) {
            return {};
        }
        const bool sameSource = source == nextSource;
        source = nextSource;
        sequence = nextSequence;
        if (current == nullptr) {
            known = false;
            hasCounter = false;
            return {Continuity::invalid, true, false};
        }
        if (sameSource && hasCounter && nextHasCounter && counter == nextCounter) {
            const bool same = snapshot == *current;
            known = known && same;
            return {same ? Continuity::duplicate : Continuity::conflict, !same, false};
        }
        Continuity continuity = Continuity::baseline;
        if (!nextHasCounter) {
            continuity = Continuity::missingCounter;
        } else if (known && sameSource && hasCounter) {
            continuity = counter == (std::numeric_limits<std::uint32_t>::max)() ? Continuity::wrap
                         : nextCounter == counter + 1 ? Continuity::consecutive
                         : nextCounter < counter      ? Continuity::reset
                                                      : Continuity::gap;
        }
        counter = nextCounter;
        hasCounter = nextHasCounter;
        known = nextHasCounter;
        snapshot = *current;
        return {continuity, true, nextHasCounter};
    }
};

} // namespace sunrise::server::activity::mission::trigger_observation
