#include "content_config_hook_storage.h"

#include <array>
#include <span>

#include "../coordinator/content_config_call_coordinator.h"

namespace sunrise::client::hooks::network::content_config {

SRWLOCK g_lifecycleLock{SRWLOCK_INIT};
std::array<hooking::detour::Handle, kHookCount> g_handles{};
std::array<void*, kHookCount> g_originals{};
std::array<void*, kHookCount> g_targetEntries{};
std::array<bool, kHookCount> g_accepting{};
void* g_contentFetch{};
bool g_trackCalls{};
volatile LONG g_activeCalls{};

} // namespace sunrise::client::hooks::network::content_config

namespace sunrise::client::hooks::network::content_config::lifecycle {

gate::Ownership g_gateOwnership{};
bool g_transitionActive{};

/** @return True when every hook handle is attached. */
bool all_installed() noexcept {
    for (const hooking::detour::Handle& handle : g_handles) {
        if (!handle.attached) {
            return false;
        }
    }
    return true;
}

/** @return True when any hook handle is attached. */
bool any_installed() noexcept {
    for (const hooking::detour::Handle& handle : g_handles) {
        if (handle.attached) {
            return true;
        }
    }
    return false;
}

/** @return True when every hook still admits observer or replacement work. */
bool all_accepting() noexcept {
    for (bool accepting : g_accepting) {
        if (!accepting) {
            return false;
        }
    }
    return true;
}

/** Clears lifecycle state. Safe only once no hook and no live call is left. */
void clear_runtime() noexcept {
    g_handles = {};
    g_originals = {};
    g_targetEntries = {};
    g_accepting = {};
    g_contentFetch = nullptr;
    g_trackCalls = false;
}

/** Removes one hook and points admitted calls at the restored target. */
bool detach(HookSlot slot, void* replacement) noexcept {
    hooking::detour::Handle& handle = g_handles[index(slot)];
    if (!handle.attached) {
        return true;
    }
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{replacement},
        hooking::detour::ProtectedCodeEntry{coordinator::ingress_entry_point()},
        hooking::detour::ProtectedCodeEntry{coordinator::egress_entry_point()},
    };
    const hooking::detour::UninstallResult result =
        hooking::detour::uninstall(handle, protectedEntries, &coordinator::idle);
    if (result != hooking::detour::UninstallResult::removed) {
        return false;
    }
    g_originals[index(slot)] = g_targetEntries[index(slot)];
    return true;
}

} // namespace sunrise::client::hooks::network::content_config::lifecycle
