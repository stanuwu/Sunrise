#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "../snapshot/snapshot.h"

namespace sunrise::core::log::view {

/** 128 bytes cap the case-insensitive text query. */
inline constexpr std::size_t kTextFilterCapacity = 128;

/** Value-owned channel, level, and ASCII text filter. */
struct Filter {
    std::optional<Channel> channel;
    std::optional<Level> level;
    std::array<char, kTextFilterCapacity> text{};
    std::size_t textLength{};
};

/** Bounded selection that borrows entries from one caller-owned logger snapshot. */
class Result final {
public:
    /**
     * Returns matching entries ordered from oldest to newest.
     * @return Pointers valid only while the source snapshot stays alive and unmoved.
     */
    [[nodiscard]] std::span<const snapshot::Entry* const> entries() const noexcept;

    /** @return Count retained by the source logger snapshot. */
    [[nodiscard]] std::size_t source_count() const noexcept;

    /** @return Count of older entries replaced before the source snapshot. */
    [[nodiscard]] std::uint64_t overwritten_count() const noexcept;

private:
    friend Result select(const snapshot::Snapshot& source, const Filter& filter) noexcept;

    std::array<const snapshot::Entry*, snapshot::kEntryCapacity> entries_{};
    std::size_t count_{};
    std::size_t sourceCount_{};
    std::uint64_t overwrittenCount_{};
};

/**
 * Replaces the bounded text query without changing it on overflow.
 * @param filter Filter that owns the query storage.
 * @param text ASCII query compared without case.
 * @return True when the complete query fits.
 */
[[nodiscard]] bool set_text(Filter& filter, std::string_view text) noexcept;

/**
 * Applies every active filter to one value-owned logger snapshot.
 * @param source Chronological logger snapshot.
 * @param filter Optional exact channel, exact level, and ASCII text query.
 * @return Bounded selection borrowing entries from source for its unchanged lifetime.
 */
[[nodiscard]] Result select(const snapshot::Snapshot& source, const Filter& filter) noexcept;

} // namespace sunrise::core::log::view
