#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>

#include "internal.h"

namespace sunrise::core::log::snapshot {
namespace {

/** One trailing byte keeps a null for debugger-friendly storage. */
constexpr std::size_t kTextTerminatorBytes = 1;

struct RingState {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<Entry, kEntryCapacity> entries{};
    std::size_t nextIndex{};
    std::size_t count{};
    std::uint64_t overwrittenCount{};
};

RingState g_ring;

/** @param channel Value to inspect. @return True for a defined log channel. */
[[nodiscard]] bool valid_channel(Channel channel) noexcept {
    return static_cast<std::size_t>(channel) < static_cast<std::size_t>(Channel::count);
}

/** @param level Value to inspect. @return True for an emitted severity. */
[[nodiscard]] bool valid_level(Level level) noexcept {
    return level >= Level::error && level < Level::off;
}

} // namespace

/** @return Channel that accepted the event. */
Channel Entry::channel() const noexcept {
    return channel_;
}

/** @return Severity that accepted the event. */
Level Entry::level() const noexcept {
    return level_;
}

/** @return Bounded formatted event text without the sink line ending. */
std::string_view Entry::text() const noexcept {
    return {text_.data(), textLength_};
}

/** @return Retained entries ordered from oldest to newest. */
std::span<const Entry> Snapshot::entries() const noexcept {
    return {entries_.data(), count_};
}

/** @return Count of older entries replaced by the fixed ring. */
std::uint64_t Snapshot::overwritten_count() const noexcept {
    return overwrittenCount_;
}

/** @return A value-owned chronological copy of all retained events. */
Snapshot take() noexcept {
    Snapshot result;
    AcquireSRWLockShared(&g_ring.lock);
    result.count_ = g_ring.count;
    result.overwrittenCount_ = g_ring.overwrittenCount;
    const std::size_t firstIndex =
        (g_ring.nextIndex + kEntryCapacity - g_ring.count) % kEntryCapacity;
    for (std::size_t index = 0; index < g_ring.count; ++index) {
        result.entries_[index] = g_ring.entries[(firstIndex + index) % kEntryCapacity];
    }
    ReleaseSRWLockShared(&g_ring.lock);
    return result;
}

namespace internal {

/** Clears retained events before a new logger lifecycle starts. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_ring.lock);
    g_ring.entries = {};
    g_ring.nextIndex = 0;
    g_ring.count = 0;
    g_ring.overwrittenCount = 0;
    ReleaseSRWLockExclusive(&g_ring.lock);
}

/**
 * Adds one already-enabled formatted event to the fixed logger ring.
 * @param channel Channel that accepted the event.
 * @param level Severity that accepted the event.
 * @param text Formatted event text without the sink line ending.
 */
void record(Channel channel, Level level, std::string_view text) noexcept {
    if (!valid_channel(channel) || !valid_level(level)) {
        return;
    }

    AcquireSRWLockExclusive(&g_ring.lock);
    Entry& entry = g_ring.entries[g_ring.nextIndex];
    entry = {};
    entry.channel_ = channel;
    entry.level_ = level;
    const std::size_t maximumText = entry.text_.size() - kTextTerminatorBytes;
    entry.textLength_ = (std::min)(text.size(), maximumText);
    if (entry.textLength_ != 0) {
        std::copy_n(text.data(), entry.textLength_, entry.text_.data());
    }
    g_ring.nextIndex = (g_ring.nextIndex + 1) % kEntryCapacity;
    if (g_ring.count < kEntryCapacity) {
        ++g_ring.count;
    } else if (g_ring.overwrittenCount != (std::numeric_limits<std::uint64_t>::max)()) {
        ++g_ring.overwrittenCount;
    }
    ReleaseSRWLockExclusive(&g_ring.lock);
}

} // namespace internal
} // namespace sunrise::core::log::snapshot
