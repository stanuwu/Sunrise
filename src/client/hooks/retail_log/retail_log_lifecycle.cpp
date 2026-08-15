#include "retail_log_lifecycle.h"

#include <array>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"
#include "retail_log_enqueue_observer.h"

namespace sunrise::client::hooks::retail_log {

SRWLOCK g_lock{SRWLOCK_INIT};
hooking::detour::Handle g_handle{};

/** Installs the game retail-log capture hook. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_handle.attached) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (!targets::game::retail_log::is_resolved()) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=retail stage=install result=fail reason=target");
        return false;
    }

    const hooking::detour::Spec spec{targets::game::retail_log::get().enqueue,
                                     enqueue_entry_point()};
    const bool installed = hooking::detour::install(spec, g_handle);
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     installed ? "ev=retail stage=install result=ok"
                               : "ev=retail stage=install result=fail reason=detour");
    return installed;
}

/** Removes the retail-log capture hook. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_handle.attached) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    // The observer body reads the trampoline without a lock, so it must be idle at detach.
    const std::array<hooking::detour::ProtectedCodeEntry, 1> protectedEntries{
        hooking::detour::ProtectedCodeEntry{enqueue_entry_point()}};
    const bool removed = hooking::detour::uninstall(g_handle, protectedEntries)
                         == hooking::detour::UninstallResult::removed;
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

/** @return True while the retail-log capture hook is attached. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool attached = g_handle.attached;
    ReleaseSRWLockShared(&g_lock);
    return attached;
}

} // namespace sunrise::client::hooks::retail_log
