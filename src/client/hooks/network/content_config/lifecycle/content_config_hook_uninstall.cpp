#include "../coordinator/content_config_call_coordinator.h"
#include "../runtime.h"
#include "content_config_hook_storage.h"

namespace sunrise::client::hooks::network::content_config {
namespace {

/** One second caps the shutdown wait, so we never spin on game-owned work. */
constexpr DWORD kQuiesceTimeoutMilliseconds = 1000;

/** Clears the transition flag. */
void finish_transition() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    lifecycle::g_transitionActive = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

/** Waits for admitted calls to leave. Clears the transition flag if the wait fails. */
[[nodiscard]] bool wait_for_idle() noexcept {
    if (coordinator::wait_for_idle(kQuiesceTimeoutMilliseconds)) {
        return true;
    }
    finish_transition();
    return false;
}

} // namespace

/** Closes admission and tears down the content-routing group. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (lifecycle::g_transitionActive) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return false;
    }
    lifecycle::g_transitionActive = true;

    if (!lifecycle::any_installed()) {
        if (coordinator::active_calls() != 0 || !gate::restore(lifecycle::g_gateOwnership)) {
            lifecycle::g_transitionActive = false;
            ReleaseSRWLockExclusive(&g_lifecycleLock);
            return false;
        }
        lifecycle::clear_runtime();
        lifecycle::g_transitionActive = false;
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return true;
    }

    g_accepting[index(HookSlot::contentTick)] = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    if (!wait_for_idle()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (coordinator::active_calls() != 0
        || !lifecycle::detach(HookSlot::contentTick, tick::entry_point())) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        finish_transition();
        return false;
    }
    g_accepting[index(HookSlot::enqueueGet)] = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    if (!wait_for_idle()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lifecycleLock);
    const bool removed = coordinator::active_calls() == 0
                         && gate::restore(lifecycle::g_gateOwnership)
                         && lifecycle::detach(HookSlot::enqueueGet, request::entry_point());
    if (removed) {
        lifecycle::clear_runtime();
    }
    lifecycle::g_transitionActive = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return removed;
}

} // namespace sunrise::client::hooks::network::content_config
