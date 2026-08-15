#include <array>

#include "../../../../targets/game.h"
#include "../runtime.h"
#include "content_config_hook_storage.h"

namespace sunrise::client::hooks::network::content_config {
namespace {

#if defined(SUNRISE_BAP_HOOK_TEST)
bool g_failNextInstall{};
#endif

/** Clears the transition flag and unlocks, so a failed install can return. */
void finish_failed_install() noexcept {
    lifecycle::g_transitionActive = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

} // namespace

/** Installs the whole hook group after turning off the signature gate. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!lifecycle::g_transitionActive && lifecycle::all_installed() && g_trackCalls
        && lifecycle::all_accepting() && g_contentFetch != nullptr
        && gate::is_disabled(lifecycle::g_gateOwnership)) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return true;
    }
    if (lifecycle::g_transitionActive || lifecycle::any_installed() || g_trackCalls
        || lifecycle::g_gateOwnership.address != nullptr) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return false;
    }
    lifecycle::g_transitionActive = true;
    ReleaseSRWLockExclusive(&g_lifecycleLock);

    AcquireSRWLockExclusive(&g_lifecycleLock);
    const targets::game::network::Targets& resolved = targets::game::network::get();
    if (resolved.httpEnqueueGet == nullptr || resolved.contentConfigFetch == nullptr
        || resolved.contentConfigTick == nullptr || resolved.contentManifestSignatureGate == nullptr
        || !gate::disable(resolved.contentManifestSignatureGate, lifecycle::g_gateOwnership)) {
        finish_failed_install();
        return false;
    }

    const std::array entryPoints{
        request::entry_point(),
        tick::entry_point(),
    };
    const std::array specs{
        hooking::detour::Spec{resolved.httpEnqueueGet, entryPoints[index(HookSlot::enqueueGet)]},
        hooking::detour::Spec{resolved.contentConfigTick,
                              entryPoints[index(HookSlot::contentTick)]},
    };
    std::array<hooking::detour::Handle, kHookCount> installed{};
    bool forcedInstallFailure{};
#if defined(SUNRISE_BAP_HOOK_TEST)
    forcedInstallFailure = g_failNextInstall;
    g_failNextInstall = false;
#endif
    if (forcedInstallFailure || !hooking::detour::install(specs, installed)) {
        (void)gate::restore(lifecycle::g_gateOwnership);
        finish_failed_install();
        return false;
    }

    g_handles = installed;
    for (std::size_t offset = 0; offset < installed.size(); ++offset) {
        g_originals[offset] = installed[offset].original;
        g_targetEntries[offset] = specs[offset].target;
        g_accepting[offset] = true;
    }
    g_contentFetch = resolved.contentConfigFetch;
    g_trackCalls = true;
    lifecycle::g_transitionActive = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return true;
}

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Makes the next hook install fail, for the lifecycle test. */
void fail_next_install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_failNextInstall = true;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network::content_config
