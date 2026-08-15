#include <Windows.h>

#include <imgui.h>

#include "../allocator.h"
#include "internal.h"

namespace sunrise::core::ui::memory {
namespace {

ImGuiMemAllocFunc g_previousAllocate{};
ImGuiMemFreeFunc g_previousRelease{};
void* g_previousUserData{};
bool g_installed{};
SRWLOCK g_allocatorLock{SRWLOCK_INIT};

/**
 * Dear ImGui allocation callback. The lock orders it against lifecycle and stats reads.
 * @param size Requested payload bytes.
 * @param userData Unused because the arena is process-static.
 * @return Aligned arena payload, or null when the arena is full.
 */
[[nodiscard]] void* allocate_callback(std::size_t size, void* userData) noexcept {
    (void)userData;
    AcquireSRWLockExclusive(&g_allocatorLock);
    void* result = g_installed ? fixed::allocate(size) : nullptr;
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return result;
}

/**
 * Dear ImGui release callback. Merges the block with its physical neighbors.
 * @param pointer Payload previously returned by allocate_callback, or null.
 * @param userData Unused because the arena is process-static.
 */
void release_callback(void* pointer, void* userData) noexcept {
    (void)userData;
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (g_installed) {
        fixed::release(pointer);
    }
    ReleaseSRWLockExclusive(&g_allocatorLock);
}

} // namespace

/** Installs the fixed allocator before any Dear ImGui context exists. */
bool initialize() noexcept {
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (g_installed) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return true;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return false;
    }

    ImGui::GetAllocatorFunctions(&g_previousAllocate, &g_previousRelease, &g_previousUserData);
    fixed::reset();
    g_installed = true;
    ImGui::SetAllocatorFunctions(allocate_callback, release_callback, nullptr);
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return true;
}

/** Restores the earlier Dear ImGui allocator once every owned allocation is freed. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (!g_installed) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return true;
    }
    if (fixed::snapshot().outstandingAllocations != 0) {
        // Restoring callbacks early would send later releases to the wrong allocator.
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return false;
    }

    ImGui::SetAllocatorFunctions(g_previousAllocate, g_previousRelease, g_previousUserData);
    fixed::clear();
    g_previousAllocate = nullptr;
    g_previousRelease = nullptr;
    g_previousUserData = nullptr;
    g_installed = false;
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return true;
}

/** @return One copy of the allocator counters, read under the lock. */
Stats snapshot() noexcept {
    AcquireSRWLockShared(&g_allocatorLock);
    const fixed::ArenaStats arena = fixed::snapshot();
    const Stats result{g_installed,
                       kArenaCapacityBytes,
                       arena.outstandingAllocations,
                       arena.outstandingBytes,
                       arena.highWaterBytes,
                       g_installed ? arena.largestFreeBytes : 0};
    ReleaseSRWLockShared(&g_allocatorLock);
    return result;
}

} // namespace sunrise::core::ui::memory
