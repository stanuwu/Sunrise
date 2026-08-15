#include <Windows.h>

#include <array>
#include <atomic>

#include "../../../core/logging/log.h"
#include "polled_input_replacements.h"
#include "runtime.h"

namespace sunrise::client::hooks::polled_input {
namespace {

/** Loading from System32 stops the guard taking exports from the game directory. */
constexpr wchar_t kUserModule[] = L"user32.dll";
/** Export reporting the key state of the calling thread's message queue. */
constexpr char kGetKeyStateExport[] = "GetKeyState";
/** Export reporting the key state at the time of the call. */
constexpr char kGetAsyncKeyStateExport[] = "GetAsyncKeyState";

std::atomic_bool g_installed{false};
HMODULE g_module{};

/** Frees the module we hold, only after every detour is detached. */
void release_module() noexcept {
    if (g_module != nullptr) {
        (void)FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

/** Attaches both polled key guards in one transaction. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!resolve_game_range()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=polled_input stage=install result=fail reason=image");
        return false;
    }
    g_module = LoadLibraryExW(kUserModule, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_module == nullptr) {
        clear_game_range();
        return false;
    }

    g_targets[static_cast<std::size_t>(HookSlot::getKeyState)] =
        reinterpret_cast<void*>(GetProcAddress(g_module, kGetKeyStateExport));
    g_targets[static_cast<std::size_t>(HookSlot::getAsyncKeyState)] =
        reinterpret_cast<void*>(GetProcAddress(g_module, kGetAsyncKeyStateExport));
    for (void* target : g_targets) {
        if (target == nullptr) {
            g_targets = {};
            clear_game_range();
            release_module();
            return false;
        }
    }

    const std::array specs{
        hooking::detour::Spec{g_targets[static_cast<std::size_t>(HookSlot::getKeyState)],
                              reinterpret_cast<void*>(&get_key_state)},
        hooking::detour::Spec{g_targets[static_cast<std::size_t>(HookSlot::getAsyncKeyState)],
                              reinterpret_cast<void*>(&get_async_key_state)},
    };
    // Every slot needs its spec, or the transaction silently attaches fewer hooks than it has.
    static_assert(specs.size() == kHandleCount);
    if (!hooking::detour::install(specs, g_handles)) {
        g_targets = {};
        clear_game_range();
        release_module();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=polled_input stage=install result=fail reason=attach");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=polled_input stage=install result=ok");
    return true;
}

/** Detaches both polled key guards and stops answering game reads. */
void uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        release_module();
        return;
    }
    // Reads go back to the real state before the trampoline that serves them is gone.
    apply_policy(false);
    if (!hooking::detour::uninstall(g_handles)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=polled_input stage=uninstall result=fail");
        return;
    }
    g_installed.store(false, std::memory_order_release);
    g_targets = {};
    clear_game_range();
    release_module();
}

/** @return True while both polled key guards are attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::polled_input
