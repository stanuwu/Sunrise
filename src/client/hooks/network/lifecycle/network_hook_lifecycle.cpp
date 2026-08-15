#include "../../external_server/route.h"
#include "../content_config/runtime.h"
#include "../coordinator/network_call_coordinator.h"
#include "../investment/investment_derived_rebuild.h"
#include "../platform.h"
#include "../runtime.h"
#include "network_hook_entries.h"
#include "network_hook_group.h"

namespace sunrise::client::hooks::network {
namespace {

/** One second caps the cleanup wait, so we never spin on game-owned work. */
constexpr DWORD kQuiesceTimeoutMilliseconds = 1000;

/** @return True when every outer base-network call left before the timeout. */
[[nodiscard]] bool wait_for_base_idle() noexcept {
    return coordinator::wait_for_idle(kQuiesceTimeoutMilliseconds);
}

/** Stops a group accepting calls, before we wait for the ones still running. */
void disable_group(std::span<const HookSlot> slots) noexcept {
    lifecycle::set_accepting(slots, false);
}

} // namespace

/** Installs the game-owned HTTP and transport replacements as one unit. */
bool install_game() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool baseInstalled = lifecycle::all_installed(lifecycle::kGameSlots);
    if (baseInstalled && !lifecycle::all_accepting(lifecycle::kGameSlots)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (baseInstalled && content_config::is_installed() && investment::is_installed()) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (lifecycle::any_installed(lifecycle::kGameSlots) && !baseInstalled) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (investment::has_ownership() && !investment::is_installed()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    const lifecycle::GameSpecs specs = lifecycle::game_specs();
    const bool installedBase =
        baseInstalled || lifecycle::install_group(specs, lifecycle::kGameSlots);
    const bool installedContent = installedBase && content_config::install();
    const bool installed = installedContent && investment::install();
    if (installed) {
        // External-server mode only. Unpins TLS and points request URLs at that server.
        (void)external_server::install();
    }
    if (!installed && installedBase) {
        disable_group(lifecycle::kGameSlots);
        const bool investmentRemoved = investment::uninstall();
        const bool contentRemoved = !content_config::has_ownership() || content_config::uninstall();
        bool forcedRollbackFailure{};
#if defined(SUNRISE_BAP_HOOK_TEST)
        forcedRollbackFailure = testing::consume_game_rollback_failure();
#endif
        if (investmentRemoved && contentRemoved && !forcedRollbackFailure) {
            const auto protectedEntries = lifecycle::game_protected_entries();
            (void)lifecycle::uninstall_group(lifecycle::kGameSlots, protectedEntries);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return installed;
}

/** Installs the Steam networking replacements once its module is loaded. */
bool install_platform() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (lifecycle::all_installed(lifecycle::kPlatformSlots)) {
        const bool accepting = lifecycle::all_accepting(lifecycle::kPlatformSlots);
        ReleaseSRWLockExclusive(&g_lock);
        return accepting;
    }
    if (lifecycle::any_installed(lifecycle::kPlatformSlots)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    const lifecycle::PlatformSpecs specs = lifecycle::platform_specs();
    const bool installed = lifecycle::install_group(specs, lifecycle::kPlatformSlots);
    ReleaseSRWLockExclusive(&g_lock);
    return installed;
}

/** Removes platform hooks before game hooks. */
bool uninstall() noexcept {
    if (coordinator::current_thread_active()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lock);
    disable_group(lifecycle::kPlatformSlots);
    disable_group(lifecycle::kGameSlots);
    ReleaseSRWLockExclusive(&g_lock);
    if (!wait_for_base_idle()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lock);
    const auto platformProtectedEntries = lifecycle::platform_protected_entries();
    if (coordinator::active_calls() != 0
        || !lifecycle::uninstall_group(lifecycle::kPlatformSlots, platformProtectedEntries)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    ReleaseSRWLockExclusive(&g_lock);

    external_server::uninstall();
    if (!investment::uninstall() || !content_config::uninstall() || !wait_for_base_idle()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lock);
    if (coordinator::active_calls() != 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const auto gameProtectedEntries = lifecycle::game_protected_entries();
    const bool removed = lifecycle::uninstall_group(lifecycle::kGameSlots, gameProtectedEntries);
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

/** @return True only when every game-owned networking detour accepts calls. */
bool is_game_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = lifecycle::all_installed(lifecycle::kGameSlots)
                           && lifecycle::all_accepting(lifecycle::kGameSlots)
                           && content_config::is_installed() && investment::is_installed();
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

/** @return True while the game-owned group still needs cleanup. */
bool has_game_ownership() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool owned = lifecycle::any_installed(lifecycle::kGameSlots)
                       || content_config::has_ownership() || investment::has_ownership()
                       || coordinator::active_calls() != 0;
    ReleaseSRWLockShared(&g_lock);
    return owned;
}

/** @return True only when both platform detours accept calls. */
bool is_platform_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = lifecycle::all_installed(lifecycle::kPlatformSlots)
                           && lifecycle::all_accepting(lifecycle::kPlatformSlots);
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

/** @return True only when both networking hook groups accept calls. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = lifecycle::all_installed(lifecycle::kGameSlots)
                           && lifecycle::all_accepting(lifecycle::kGameSlots)
                           && lifecycle::all_installed(lifecycle::kPlatformSlots)
                           && lifecycle::all_accepting(lifecycle::kPlatformSlots)
                           && content_config::is_installed() && investment::is_installed();
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

} // namespace sunrise::client::hooks::network
