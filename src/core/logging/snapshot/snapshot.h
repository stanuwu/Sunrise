#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../log.h"

namespace sunrise::core::log::snapshot {

/** The newest 128 events cover short diagnosis windows and cap static storage. */
inline constexpr std::size_t kEntryCapacity = 128;

namespace internal {

/** Stores one already-enabled formatted event in the fixed logger ring. */
void record(Channel channel, Level level, std::string_view text) noexcept;

} // namespace internal

/** One immutable structured event copied out of the logger ring. */
class Entry final {
public:
    /** @return Channel that accepted the event. */
    [[nodiscard]] Channel channel() const noexcept;

    /** @return Severity that accepted the event. */
    [[nodiscard]] Level level() const noexcept;

    /** @return Bounded formatted event text without the sink line ending. */
    [[nodiscard]] std::string_view text() const noexcept;

private:
    friend void internal::record(Channel channel, Level level, std::string_view text) noexcept;

    Channel channel_{Channel::core};
    Level level_{Level::error};
    std::array<char, kLineCapacity> text_{};
    std::size_t textLength_{};
};

/** Value-owned chronological copy of the bounded logger ring. */
class Snapshot final {
public:
    /** @return Retained entries ordered from oldest to newest. */
    [[nodiscard]] std::span<const Entry> entries() const noexcept;

    /** @return Count of older entries replaced by the fixed ring. */
    [[nodiscard]] std::uint64_t overwritten_count() const noexcept;

private:
    friend Snapshot take() noexcept;

    std::array<Entry, kEntryCapacity> entries_{};
    std::size_t count_{};
    std::uint64_t overwrittenCount_{};
};

/** @return A value-owned chronological copy of all retained events. */
[[nodiscard]] Snapshot take() noexcept;

} // namespace sunrise::core::log::snapshot
