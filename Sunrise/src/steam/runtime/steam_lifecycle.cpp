#include <Windows.h>

#include <atomic>

#include "../../client/graphics/wine_compat.h"
#include "../../client/hooks/bootflow/bootflow_texture_override.h"
#include "../../client/hooks/egress/runtime.h"
#include "../../client/hooks/package_trust/package_trust_bypass.h"
#include "../../client/runtime/runtime.h"
#include "../../core/logging/log.h"
#include "../../core/runtime/core_runtime.h"
#include "../../core/runtime/host_environment.h"
#include "../../core/settings/role.h"
#include "../../state/social/steam_roster.h"
#include "../interfaces/internal.h"
#include "callbacks/callback_registry.h"
#include "core/threading/data_mutex.h"
#include "internal.h"
#include "runtime.h"
#include "steam_context_state.h"

namespace sunrise::steam {
namespace {

/** The only delay-loaded module allowed to start the platform Client group. */
constexpr wchar_t kNetworkingModuleName[] = L"steamnetworkingsockets.dll";

/** One-shot activation receipts, so each Client group is stood up exactly once per run. */
struct Lifecycle {
    bool mainActivationDone{};
    bool mainActivationResult{};
    bool graphicsActivationAttempted{};
    bool platformActivationAttempted{};
};

core::threading::SharedDataMutex<Lifecycle> g_lifecycle;
std::atomic_bool g_initialized{false};

/**
 * Finds the loaded image that owns a caught return address. It takes no module reference.
 * @return The owning module, or null when it is not the Steam networking image.
 */
[[nodiscard]] HMODULE networking_caller_module(const void* callerAddress) noexcept {
    if (callerAddress == nullptr) {
        return nullptr;
    }
    HMODULE callerModule{};
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(callerAddress), &callerModule) == FALSE
        || callerModule != GetModuleHandleW(kNetworkingModuleName)) {
        return nullptr;
    }
    return callerModule;
}

} // namespace

/** Starts the Steam shim and its exported state. */
bool initialize(void* module) noexcept {
    if (!core::settings::activates_client_hooks(core::settings::role())) {
        return false;
    }
    if (core::runtime::is_wine()) {
        client::graphics::initialize_wine_display();
    }
    if (!client::hooks::egress::install()) {
        return false;
    }

    return g_lifecycle.lock_write([module](Lifecycle&) {
        if (g_initialized.load(std::memory_order_acquire)) {
            return true;
        }
        if (!core::initialize(module)) {
            return false;
        }
        // Base generation (_0) packages register during bootload, before the first callback pump,
        // so package trust must attach at Steam init rather than in the main-image hook sweep.
        if (!client::hooks::package_trust::install()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             "ev=steam_init stage=package_trust result=fail");
            (void)core::shutdown();
            return false;
        }
        // Bootflow GPU entries can load before the first Steam callback pump. The decoded-entry
        // override must therefore attach here while the stock `_unp1` package remains registered
        // through its native path.
        if (!client::hooks::bootflow::texture_override::install(module)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=steam_init stage=bootflow_texture result=fail");
        }
        advance_context_generation();
        g_initialized.store(true, std::memory_order_release);
        core::log::write(
            core::log::Channel::client, core::log::Level::info, "ev=steam_init result=ok");
        // The guard attaches above, before Core logging exists, so its outcome is reported here.
        client::hooks::egress::report_installation();
        return true;
    });
}

/** Stops callback delivery and clears Steam state. */
bool shutdown() noexcept {
    return g_lifecycle.lock_write([](Lifecycle& lifecycle) {
        const bool hadRuntime =
            g_initialized.load(std::memory_order_acquire) || core::is_initialized();
        if (!hadRuntime) {
            return true;
        }
        // Invalidate entering social passes and queued events before their producers stop.
        interfaces::methods::reset_friends();
        if (!core::shutdown()) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             "ev=steam_shutdown stage=core result=fail");
            return false;
        }

        // Callback pointers are released only after Client hooks stop producing events.
        runtime::callbacks::clear();
        state::social::client_disconnected();
        state::social::lobby::reset();
        g_initialized.store(false, std::memory_order_release);
        lifecycle.mainActivationDone = false;
        lifecycle.mainActivationResult = false;
        lifecycle.graphicsActivationAttempted = false;
        lifecycle.platformActivationAttempted = false;
        advance_context_generation();
        return true;
    });
}

/** @return True. The in-process Steam provider stays up for the whole DLL lifetime. */
bool is_running() noexcept {
    return true;
}

} // namespace sunrise::steam

namespace sunrise::steam::runtime {

/** Runs main-image activation once, from a caller that proves the game is loaded. */
bool activate_main_once() noexcept {
    if (!core::settings::activates_client_hooks(core::settings::role())) {
        return false;
    }
    return g_lifecycle.lock_write([](Lifecycle& lifecycle) {
        if (!lifecycle.mainActivationDone && core::is_initialized()) {
            lifecycle.mainActivationDone = true;
            lifecycle.mainActivationResult = client::activate_main_once();
        }
        return lifecycle.mainActivationResult;
    });
}

/** @return True while the main-image sweep has not run yet. */
bool main_activation_pending() noexcept {
    const bool pending = g_lifecycle.lock_read(
        [](const Lifecycle& lifecycle) { return !lifecycle.mainActivationDone; });
    // Matches the activation's Core test. A failed Core must not raise an endless overlay.
    return pending && core::is_initialized();
}

/** Installs the presentation hooks once, from the callback pump, before the game sweep. */
void activate_graphics_once() noexcept {
    g_lifecycle.lock_write([](Lifecycle& lifecycle) {
        if (!lifecycle.graphicsActivationAttempted && core::is_initialized()) {
            lifecycle.graphicsActivationAttempted = true;
            (void)client::activate_graphics_once();
        }
    });
}

/** Activates the platform Client group at its exact interface request boundary. */
void activate_platform_once(const void* callerAddress) noexcept {
    const HMODULE callerModule = networking_caller_module(callerAddress);
    if (callerModule == nullptr) {
        return;
    }

    g_lifecycle.lock_write([callerModule](Lifecycle& lifecycle) {
        if (!lifecycle.platformActivationAttempted
            && g_initialized.load(std::memory_order_acquire)) {
            lifecycle.platformActivationAttempted = true;
            (void)client::activate_platform_once(callerModule);
        }
    });
}

} // namespace sunrise::steam::runtime
