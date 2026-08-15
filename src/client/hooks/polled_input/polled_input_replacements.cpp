/**
 * Guards on the two polled key exports. The game scans GetKeyState over its whole key table every
 * frame, which no window procedure can see, so a visible interface must answer those reads itself.
 * Only calls from the game image are answered. Dear ImGui reads the real state for its modifiers.
 */

#include "polled_input_replacements.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <intrin.h>

#include "../../diagnostics/module_range.h"
#include "runtime.h"

namespace sunrise::client::hooks::polled_input {
namespace {

std::atomic_bool g_interfaceOpen{};
diagnostics::ModuleRange g_gameRange{};
/** One key reported held to game code, so an action can be driven through the game's own scan. */
std::atomic_uint32_t g_forcedKey{kNoForcedKey};

/**
 * @param caller Return address of the call being answered.
 * @return True when the caller is game code rather than Sunrise or Dear ImGui.
 */
[[nodiscard]] bool caller_is_game(const void* caller) noexcept {
    return diagnostics::contains(g_gameRange, reinterpret_cast<std::uintptr_t>(caller));
}

/**
 * @param virtualKey Key the game is asking about.
 * @return True while this key is being reported held on the game's behalf.
 */
[[nodiscard]] bool is_forced(int virtualKey) noexcept {
    const std::uint32_t forced = g_forcedKey.load(std::memory_order_relaxed);
    return forced != kNoForcedKey && static_cast<std::uint32_t>(virtualKey) == forced;
}

} // namespace

std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::array<void*, kHandleCount> g_targets{};

/**
 * Reports every key released to game code while the interface is open.
 * The scan writes an up-bit from a zero result, so held keys release instead of latching down.
 * @param virtualKey Windows virtual-key code.
 * @return Released while the interface is open and the caller is the game, else the real state.
 */
__declspec(noinline) SHORT WINAPI get_key_state(int virtualKey) noexcept {
    if (caller_is_game(_ReturnAddress())) {
        if (g_interfaceOpen.load(std::memory_order_relaxed)) {
            return kKeyReleased;
        }
        if (is_forced(virtualKey)) {
            return kKeyHeld;
        }
    }
    const GetKeyState next = original<GetKeyState>(HookSlot::getKeyState);
    if (next == nullptr) {
        return kKeyReleased;
    }
    return next(virtualKey);
}

/**
 * Reports every key released to game code while the interface is open.
 * @param virtualKey Windows virtual-key code.
 * @return Released while the interface is open and the caller is the game, else the real state.
 */
__declspec(noinline) SHORT WINAPI get_async_key_state(int virtualKey) noexcept {
    if (g_interfaceOpen.load(std::memory_order_relaxed) && caller_is_game(_ReturnAddress())) {
        return kKeyReleased;
    }
    const GetAsyncKeyState next = original<GetAsyncKeyState>(HookSlot::getAsyncKeyState);
    if (next == nullptr) {
        return kKeyReleased;
    }
    return next(virtualKey);
}

/** Finds the game image range the caller test uses. */
bool resolve_game_range() noexcept {
    return diagnostics::module_range(GetModuleHandleW(nullptr), g_gameRange);
}

/** Drops the game image range. */
void clear_game_range() noexcept {
    g_gameRange = diagnostics::ModuleRange{};
}

/** Records an interface visibility change for the polled reads. */
void apply_policy(bool visible) noexcept {
    g_interfaceOpen.store(visible, std::memory_order_relaxed);
}

/** Reports one key held to game code until it is released. */
void hold_key(std::uint32_t virtualKey) noexcept {
    g_forcedKey.store(virtualKey, std::memory_order_relaxed);
}

/** Stops reporting any key held on the game's behalf. */
void release_key() noexcept {
    g_forcedKey.store(kNoForcedKey, std::memory_order_relaxed);
}

/** Applies the polled-input policy for the current interface visibility. */
void apply_visibility(bool visible) noexcept {
    apply_policy(visible);
}

} // namespace sunrise::client::hooks::polled_input
