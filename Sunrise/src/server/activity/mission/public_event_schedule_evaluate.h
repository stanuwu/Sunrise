#pragma once

#include <cstdint>

#include "../../../state/build_data/public_event_schedule/definition.h"

namespace sunrise::server::activity::mission::public_event_schedule {
using Table = state::build_data::public_event_schedule::Table;
using Row = state::build_data::public_event_schedule::Row;

/** Where a row stands at one instant: counting down to a start, or inside its window. */
struct Occurrence {
    /** True only inside this occurrence's start/finish window. */
    bool active{};
    /** Whole seconds until start; zero while active. */
    std::int64_t untilStart{};
    /** Unix seconds at the unjittered slot-grid boundary. */
    std::int64_t slotStart{};
    /** Unix seconds at this slot's jittered start. */
    std::int64_t start{};
    /** Unix seconds at the end of its active window. */
    std::int64_t finish{};
    /** Slot index reduced to the client's 365-day rotation grid. */
    std::uint64_t slot{};
};

/** One site's view of its row: the occurrence and the entry the slot picks. */
struct SiteResult {
    /** The row's current active or next future opportunity. */
    Occurrence occurrence{};
    /** Index of the first authored row listing the requested site. */
    std::size_t row{};
    /** Picked entry index within the table, or kNoEntry. */
    std::size_t picked{};
    /** The weighted slot pick matches the requested site. */
    bool selected{};
};
/** Sentinel returned when a row has no eligible positive-weight pick. */
inline constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);

/** Hashes the eight little-endian bytes of a value as the client's slot hash does. */
[[nodiscard]] std::uint32_t hash8(std::uint64_t value) noexcept;

/**
 * Evaluates one row at a wall-clock instant, as the client's Director does.
 * @param now Unix seconds.
 * @return False when the row schedules nothing.
 */
[[nodiscard]] bool evaluate(const Row& row, std::int64_t now, Occurrence& out) noexcept;

/** @return The weighted entry the slot picks, as a table index, or kNoEntry. */
[[nodiscard]] std::size_t pick(const Table& table, const Row& row, std::uint64_t slot) noexcept;

/** @return False when no row lists the site; the first listing row decides. */
[[nodiscard]] bool
for_site(const Table& table, std::uint32_t site, std::int64_t now, SiteResult& out) noexcept;
} // namespace sunrise::server::activity::mission::public_event_schedule
