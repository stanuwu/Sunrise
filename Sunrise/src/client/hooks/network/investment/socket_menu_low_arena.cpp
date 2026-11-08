#include "socket_menu_low_arena.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include "socket_menu_native_array.h"

namespace sunrise::client::hooks::network::investment::low_arena {
namespace {

using native_array::image_pointer;

/** Relocated arrays must sit at addresses that fit 32 bits. */
constexpr std::uintptr_t kMaximumLowAddress = UINT32_MAX;
/** Bytes of the one low arena we allocate for relocated plug-set arrays. */
constexpr std::size_t kLowArenaBytes = 64U * 1024U;
/** Every payload starts on this boundary. */
constexpr std::size_t kArenaAlignment = 16U;

std::byte* g_lowArena{};
std::size_t g_lowArenaUsed{};

[[nodiscard]] std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept {
    const std::uintptr_t mask = alignment - 1U;
    if (value > (std::numeric_limits<std::uintptr_t>::max)() - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

/** Reserves one process-lifetime arena while the low content address domain is still available. */
[[nodiscard]] std::byte* reserve_low_arena() noexcept {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uintptr_t granularity = system.dwAllocationGranularity;
    std::uintptr_t cursor =
        align_up(reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress), granularity);
    while (cursor != 0 && cursor <= kMaximumLowAddress
           && kLowArenaBytes <= kMaximumLowAddress - cursor + 1U) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(image_pointer(cursor), &information, sizeof information) == 0) {
            break;
        }
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const std::uintptr_t next =
            information.RegionSize <= (std::numeric_limits<std::uintptr_t>::max)() - base
                ? base + information.RegionSize
                : 0;
        if (information.State == MEM_FREE) {
            const std::uintptr_t candidate = align_up((std::max)(cursor, base), granularity);
            const std::uintptr_t offset = candidate >= base ? candidate - base : 0;
            if (candidate != 0 && candidate <= kMaximumLowAddress
                && kLowArenaBytes <= kMaximumLowAddress - candidate + 1U && candidate >= base
                && offset <= information.RegionSize
                && kLowArenaBytes <= information.RegionSize - offset) {
                void* const allocated = VirtualAlloc(image_pointer(candidate),
                                                     kLowArenaBytes,
                                                     MEM_RESERVE | MEM_COMMIT,
                                                     PAGE_READWRITE);
                if (allocated != nullptr) {
                    return static_cast<std::byte*>(allocated);
                }
            }
        }
        if (next == 0 || next <= cursor) {
            break;
        }
        cursor = align_up(next, granularity);
    }
    return nullptr;
}

} // namespace

void reserve() noexcept {
    if (g_lowArena == nullptr) {
        g_lowArena = reserve_low_arena();
    }
}

Allocation allocate(std::size_t size) noexcept {
    const std::size_t aligned = static_cast<std::size_t>(align_up(g_lowArenaUsed, kArenaAlignment));
    if (g_lowArena == nullptr || aligned > kLowArenaBytes || size > kLowArenaBytes - aligned) {
        return {};
    }
    g_lowArenaUsed = aligned + size;
    return {g_lowArena + aligned, size};
}

} // namespace sunrise::client::hooks::network::investment::low_arena
