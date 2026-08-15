#pragma once

#include <cstddef>

namespace sunrise::core::ui::memory::fixed {

/** Internal counters read only while the public allocator lock is held. */
struct ArenaStats {
    std::size_t outstandingAllocations{};
    std::size_t outstandingBytes{};
    std::size_t highWaterBytes{};
    std::size_t largestFreeBytes{};
};

/** Rebuilds one complete free span after every prior allocation is released. */
void reset() noexcept;

/** Clears inactive arena bytes and lifecycle counters. */
void clear() noexcept;

/**
 * Takes the first free span that holds one aligned request.
 * @param requestedBytes Exact payload bytes requested by Dear ImGui.
 * @return Aligned arena payload, or null when the arena is full.
 */
[[nodiscard]] void* allocate(std::size_t requestedBytes) noexcept;

/** @param pointer Payload previously returned by allocate, or null. */
void release(void* pointer) noexcept;

/** @return Current counters and the largest merged free payload. */
[[nodiscard]] ArenaStats snapshot() noexcept;

} // namespace sunrise::core::ui::memory::fixed
