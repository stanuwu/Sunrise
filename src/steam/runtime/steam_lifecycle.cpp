#include <Windows.h>

#include <atomic>

#include "../../client/hooks/egress/runtime.h"
#include "../../client/runtime/runtime.h"
#include "../../core/logging/log.h"
#include "../../core/runtime/core_runtime.h"
#include "callbacks/callback_registry.h"
#include "context/steam_context_state.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::steam {
namespace {

/** The only delay-loaded module allowed to start the platform Client group. */
constexpr wchar_t kNetworkingModuleName[] = L"steamnetworkingsockets.dll";

SRWLOCK g_lifecycleLock{SRWLOCK_INIT};
std::atomic_bool g_initialized{false};
bool g_mainActivationDone{};
bool g_mainActivationResult{};
bool g_graphicsActivationAttempted{};
bool g_platformActivationAttempted{};

/**
 * Finds the loaded image that owns a caught return address. It takes no reference on the module.
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
    if (!client::hooks::egress::install()) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (g_initialized.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return true;
    }
    if (!core::initialize(module)) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return false;
    }
    context::advance_generation();
    g_initialized.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=steam_init result=ok");
    // The guard attaches above, before Core logging exists, so its outcome is reported here.
    client::hooks::egress::report_installation();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return true;
}

/** Stops callback delivery and clears Steam state. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    const bool hadRuntime = g_initialized.load(std::memory_order_acquire) || core::is_initialized();
    if (!hadRuntime) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return true;
    }
    if (!core::shutdown()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=steam_shutdown stage=core result=fail");
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return false;
    }

    // Callback pointers are released only after Client hooks stop producing events.
    runtime::callbacks::clear();
    g_initialized.store(false, std::memory_order_release);
    g_mainActivationDone = false;
    g_mainActivationResult = false;
    g_graphicsActivationAttempted = false;
    g_platformActivationAttempted = false;
    context::advance_generation();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return true;
}

/** @return True. The in-process Steam provider stays up for the whole DLL lifetime. */
bool is_running() noexcept {
    return true;
}

} // namespace sunrise::steam

namespace sunrise::steam::runtime {

/** Runs main-image activation once, from a caller that proves the game is loaded. */
bool activate_main_once() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!g_mainActivationDone && core::is_initialized()) {
        g_mainActivationDone = true;
        g_mainActivationResult = client::activate_main_once();
    }
    const bool result = g_mainActivationResult;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return result;
}

/** @return True while the main-image sweep has not run yet. */
bool main_activation_pending() noexcept {
    AcquireSRWLockShared(&g_lifecycleLock);
    const bool pending = !g_mainActivationDone;
    ReleaseSRWLockShared(&g_lifecycleLock);
    // This Core test matches the one the activation makes. Without it, a failed Core raises an
    // overlay that no sweep ever ends.
    return pending && core::is_initialized();
}

/** Installs the presentation hooks once, from the callback pump, before the game sweep. */
void activate_graphics_once() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!g_graphicsActivationAttempted && core::is_initialized()) {
        g_graphicsActivationAttempted = true;
        (void)client::activate_graphics_once();
    }
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

/** Activates the platform Client group at its exact interface request boundary. */
void activate_platform_once(const void* callerAddress) noexcept {
    const HMODULE callerModule = networking_caller_module(callerAddress);
    if (callerModule == nullptr) {
        return;
    }

    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!g_platformActivationAttempted && g_initialized.load(std::memory_order_acquire)) {
        g_platformActivationAttempted = true;
        (void)client::activate_platform_once(callerModule);
    }
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

} // namespace sunrise::steam::runtime
