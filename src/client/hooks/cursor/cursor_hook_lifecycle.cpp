#include <Windows.h>

#include <array>
#include <atomic>

#include "../../../core/logging/log.h"
#include "cursor_guard_replacements.h"
#include "runtime.h"

namespace sunrise::client::hooks::cursor {
namespace {

/** The System32 name keeps the guard from finding exports in the game directory. */
constexpr wchar_t kUserModule[] = L"user32.dll";
/** Export that moves the pointer. */
constexpr char kSetCursorPosExport[] = "SetCursorPos";
/** Export that confines the pointer to a rectangle. */
constexpr char kClipCursorExport[] = "ClipCursor";

std::atomic_bool g_installed{false};
HMODULE g_module{};

/** Releases the retained module only after every detour is detached. */
void release_module() noexcept {
    if (g_module != nullptr) {
        (void)FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

/**
 * Attaches both cursor guards in one transaction.
 * @return True when both guards attached.
 */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    g_module = LoadLibraryExW(kUserModule, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_module == nullptr) {
        return false;
    }

    g_targets[static_cast<std::size_t>(HookSlot::setCursorPos)] =
        reinterpret_cast<void*>(GetProcAddress(g_module, kSetCursorPosExport));
    g_targets[static_cast<std::size_t>(HookSlot::clipCursor)] =
        reinterpret_cast<void*>(GetProcAddress(g_module, kClipCursorExport));
    for (void* target : g_targets) {
        if (target == nullptr) {
            g_targets = {};
            release_module();
            return false;
        }
    }

    const std::array specs{
        hooking::detour::Spec{g_targets[static_cast<std::size_t>(HookSlot::setCursorPos)],
                              reinterpret_cast<void*>(&set_cursor_pos)},
        hooking::detour::Spec{g_targets[static_cast<std::size_t>(HookSlot::clipCursor)],
                              reinterpret_cast<void*>(&clip_cursor)},
    };
    // Every slot needs its spec, or the transaction silently attaches fewer hooks than it has.
    static_assert(specs.size() == kHandleCount);
    if (!hooking::detour::install(specs, g_handles)) {
        g_targets = {};
        release_module();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=cursor stage=install result=fail");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=cursor stage=install result=ok");
    return true;
}

/** Detaches both guards and restores the confinement the game last asked for. */
void uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        release_module();
        return;
    }
    // The game's own bounds go back before the trampoline that applies them is gone.
    apply_policy(false);
    if (!hooking::detour::uninstall(g_handles)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=cursor stage=uninstall result=fail");
        return;
    }
    g_installed.store(false, std::memory_order_release);
    g_targets = {};
    release_module();
}

/** @return True while both cursor guards are attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::cursor
