#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission::trigger_observation {

namespace sense = middleware::bap::activity_message::sense_update;
/** Type-30 occupancy Sense: two required flags, then two required signed 32-bit values. */
constexpr std::uint32_t kSchema = 0x80809531U;
constexpr std::uint8_t kSlotType = 30;

/** One complete occupancy report. */
struct Snapshot final {
    bool occupied{};
    bool all{};
    std::int32_t count{};
    std::int32_t threshold{};
};

/** Reads one occupancy body. An incomplete or ambiguous body is refused, never defaulted. */
[[nodiscard]] inline bool read(std::span<const sense::DecodedValue> body,
                               Snapshot& output) noexcept {
    if (body.size() != 4) {
        return false;
    }
    std::array<const sense::DecodedValue*, 4> fields{};
    for (const auto& value : body) {
        if (value.schemaRow != kSchema || value.fieldOrdinal >= fields.size()
            || value.occurrence != 0 || !value.present || fields[value.fieldOrdinal]) {
            return false;
        }
        if (value.fieldOrdinal < 2) {
            if (value.kind != sense::ValueKind::boolean || value.width != 1
                || value.unsignedValue > 1) {
                return false;
            }
        } else if (value.kind != sense::ValueKind::signedInteger || value.width != 32
                   || value.signedValue < (std::numeric_limits<std::int32_t>::min)()
                   || value.signedValue > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        fields[value.fieldOrdinal] = &value;
    }
    output = {fields[0]->unsignedValue != 0,
              fields[1]->unsignedValue != 0,
              static_cast<std::int32_t>(fields[2]->signedValue),
              static_cast<std::int32_t>(fields[3]->signedValue)};
    return true;
}

enum class Edge : std::uint8_t { none, entered, exited };
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
/** What one report means: an optional edge, its continuity, and whether scripts hear of it. */
struct Observation final {
    Edge edge{Edge::none};
    Continuity continuity{Continuity::invalid};
    /** Scripts receive a triggerState event for this report. */
    bool notify{};
    /** The report carried a complete level the event may expose. */
    bool available{};
};

/**
 * Tracks one volume's reports. Only consecutive record counters from one source prove an edge; a
 * repeated counter never confirms a level, and a conflicting repeat invalidates it.
 */
struct Tracker final {
    std::uint64_t source{}, sequence{};
    std::uint32_t counter{};
    bool hasCounter{}, known{}, occupied{};
    Snapshot snapshot{};
    void invalidate_levels() noexcept {
        known = false;
    }

    /** @param current The decoded level, or null when the body was malformed. */
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
        if (!current) {
            source = nextSource;
            sequence = nextSequence;
            known = false;
            hasCounter = false;
            return {Edge::none, Continuity::invalid, true, false};
        }
        if (sameSource && hasCounter && nextHasCounter && counter == nextCounter) {
            const bool same = snapshot.occupied == current->occupied && snapshot.all == current->all
                              && snapshot.count == current->count
                              && snapshot.threshold == current->threshold;
            if (!same) {
                invalidate_levels();
            }
            sequence = nextSequence;
            return {Edge::none, same ? Continuity::duplicate : Continuity::conflict, !same, false};
        }
        Continuity continuity = Continuity::baseline;
        const bool consecutive = known && sameSource && hasCounter && nextHasCounter
                                 && counter != UINT32_MAX && nextCounter == counter + 1;
        if (!nextHasCounter) {
            continuity = Continuity::missingCounter;
        } else if (consecutive) {
            continuity = Continuity::consecutive;
        } else if (sameSource && hasCounter && known) {
            continuity = counter == UINT32_MAX   ? Continuity::wrap
                         : nextCounter < counter ? Continuity::reset
                                                 : Continuity::gap;
        }
        const Edge edge = !consecutive || occupied == current->occupied ? Edge::none
                          : current->occupied                           ? Edge::entered
                                                                        : Edge::exited;
        source = nextSource;
        sequence = nextSequence;
        counter = nextCounter;
        hasCounter = nextHasCounter;
        known = nextHasCounter;
        occupied = current->occupied;
        snapshot = *current;
        return {edge, continuity, true, nextHasCounter};
    }
};

} // namespace sunrise::server::activity::mission::trigger_observation
