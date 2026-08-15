#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::polled_input {

/** Windows reports a key that is not held with a zero state. */
inline constexpr SHORT kKeyReleased = 0;
/** Windows reports a held key with the high bit set. */
inline constexpr SHORT kKeyHeld = static_cast<SHORT>(0x8000);
/** No key is being reported held on the game's behalf. */
inline constexpr std::uint32_t kNoForcedKey = 0;

/** Windows SDK ABI for GetKeyState. */
using GetKeyState = SHORT(WINAPI*)(int);
/** Windows SDK ABI for GetAsyncKeyState. */
using GetAsyncKeyState = SHORT(WINAPI*)(int);

/** Slots shared by the exports and their Detours handles. */
enum class HookSlot : std::size_t {
    getKeyState,
    getAsyncKeyState,
    count,
};

/** Both polled key exports form one atomic transaction. */
inline constexpr std::size_t kHandleCount = static_cast<std::size_t>(HookSlot::count);

extern std::array<hooking::detour::Handle, kHandleCount> g_handles;
extern std::array<void*, kHandleCount> g_targets;

/**
 * Gives the live trampoline, or the restored export while detaching.
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

/** Reports every key released to game code while the interface is open. */
SHORT WINAPI get_key_state(int virtualKey) noexcept;

/** Reports every key released to game code while the interface is open. */
SHORT WINAPI get_async_key_state(int virtualKey) noexcept;

/**
 * Finds the game image range the caller test uses. An empty range contains nothing, so the guard
 * forwards every call instead of blocking it.
 * @return True when the main image headers gave a range.
 */
[[nodiscard]] bool resolve_game_range() noexcept;

/** Drops the game image range. */
void clear_game_range() noexcept;

/**
 * Records an interface visibility change for the polled reads.
 * @param visible Current Core visibility state.
 */
void apply_policy(bool visible) noexcept;

} // namespace sunrise::client::hooks::polled_input
