#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sunrise::state::build_data::public_event_schedule {
/** Row capacity above the 143 authored rows; decode refuses overflow. */
inline constexpr std::size_t kRowCapacity = 256;
/** Entry capacity above the 386 authored entries. */
inline constexpr std::size_t kEntryCapacity = 1024;
/** Destination-group capacity above the 44 authored groups. */
inline constexpr std::size_t kGroupCapacity = 64;
/** FNV offset basis: the authored value of an absent hash field. */
inline constexpr std::uint32_t kAbsentHash = 0x811C9DC5U;
/** The authored value of an entry that draws no map node. */
inline constexpr std::uint16_t kNoNode = 0xFFFFU;

/** One weighted entry of a schedule row: a public-event site a slot may pick. */
struct Entry {
    /** Authored nonnegative entry weight used by the client's float32 draw. */
    float weight{};
    /** Unlock expression gating the entry; absent on every shipped row. */
    std::uint32_t gate{kAbsentHash};
    /** Destination group; the map draws an entry only when its group is listed. */
    std::uint32_t group{};
    /** Roster key of the public-event site. */
    std::uint32_t site{};
    /** Roster key of the paired rally registry, or absent. */
    std::uint32_t rally{kAbsentHash};
    /** Destination map node the countdown is drawn at. */
    std::uint16_t node{kNoNode};
};

/** One schedule row: a periodic opportunity in whole seconds, as the client truncates them. */
struct Row {
    /** Authored row key identifying a Director schedule opportunity. */
    std::uint32_t key{};
    /** Authored salt mixed with the rotating slot index for the weighted pick. */
    std::uint32_t salt{};
    /** Whole seconds between opportunities; bounded by the rotation grid. */
    std::int64_t periodSeconds{};
    /** Maximum authored jitter span in whole seconds. */
    std::int64_t spanSeconds{};
    /** Authored slot-grid offset in whole seconds. */
    std::int64_t offsetSeconds{};
    /** Whole seconds the client reports this opportunity active. */
    std::int64_t windowSeconds{};
    /** First entry index in the enclosing table. */
    std::uint16_t firstEntry{};
    /** Number of consecutive entries owned by this row. */
    std::uint16_t entryCount{};
};

/** The decoded Director public-event schedule: rows in table order with their entries. */
struct Table {
    /** Source package tag of the decoded schedule. */
    std::uint32_t tag{};
    /** Byte length declared by the source payload. */
    std::uint32_t payloadBytes{};
    /** Number of decoded rows present in rows. */
    std::size_t rowCount{};
    /** Number of decoded entries present in entries. */
    std::size_t entryCount{};
    /** Number of authored destination groups present in groups. */
    std::size_t groupCount{};
    /** Schedule rows in authored order. */
    std::array<Row, kRowCapacity> rows{};
    /** Weighted entries shared by the bounded row ranges. */
    std::array<Entry, kEntryCapacity> entries{};
    /** Destination groups for which the client may draw a node. */
    std::array<std::uint32_t, kGroupCapacity> groups{};

    /** @return The entries of one row, in authored order. */
    [[nodiscard]] std::span<const Entry> entries_of(const Row& row) const noexcept {
        return std::span(entries).subspan(row.firstEntry, row.entryCount);
    }
};

/** Owns an immutable table for the full lifetime of a reader. */
using SnapshotView = std::shared_ptr<const Table>;

/** Seconds in the client's 365-day slot rotation; periods cannot exceed it. */
inline constexpr std::int64_t kRotationSeconds = 31536000;

/** Publishes the decoded table once from the investment worker. */
[[nodiscard]] bool publish(const Table& value) noexcept;
/** @return True once a table was published. */
[[nodiscard]] bool ready() noexcept;
/** @return An owned immutable table, empty before the first publication. */
[[nodiscard]] SnapshotView snapshot() noexcept;
} // namespace sunrise::state::build_data::public_event_schedule
