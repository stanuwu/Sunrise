#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::cursor {

/** Windows cursor calls report success with a nonzero result. */
inline constexpr BOOL kCallSucceeded = TRUE;

/** Windows SDK ABI for SetCursorPos. */
using SetCursorPos = BOOL(WINAPI*)(int, int);
/** Windows SDK ABI for ClipCursor. */
using ClipCursor = BOOL(WINAPI*)(const RECT*);

/** Stable slots shared by the exports and their Detours handles. */
enum class HookSlot : std::size_t {
    setCursorPos,
    clipCursor,
    count,
};

/** Both cursor exports form one atomic transaction. */
inline constexpr std::size_t kHandleCount = static_cast<std::size_t>(HookSlot::count);

extern std::array<hooking::detour::Handle, kHandleCount> g_handles;
extern std::array<void*, kHandleCount> g_targets;

/**
 * Returns the active trampoline, or the restored export during detachment.
 * @tparam Function Exact export ABI.
 * @return Callable entry for the current attach state, or null before install.
 */
template <typename Function> [[nodiscard]] Function original(HookSlot slot) noexcept {
    const auto index = static_cast<std::size_t>(slot);
    void* entry = g_handles[index].original;
    if (entry == nullptr) {
        entry = g_targets[index];
    }
    return reinterpret_cast<Function>(entry);
}

/** Answers the game's cursor move while the interface is open. */
BOOL WINAPI set_cursor_pos(int x, int y) noexcept;

/** Frees the pointer while the interface is open and remembers the game's own bounds. */
BOOL WINAPI clip_cursor(const RECT* bounds) noexcept;

/**
 * Applies one interface-visibility edge to the pointer.
 * Must run outside every presentation lock because it calls Win32.
 * @param visible Current Core visibility state.
 */
void apply_policy(bool visible) noexcept;

} // namespace sunrise::client::hooks::cursor
