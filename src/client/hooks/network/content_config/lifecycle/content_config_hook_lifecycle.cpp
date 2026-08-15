#include "../coordinator/content_config_call_coordinator.h"
#include "../runtime.h"
#include "content_config_hook_storage.h"

namespace sunrise::client::hooks::network::content_config {

/** @return True when every hook is in place and the signature gate is off. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lifecycleLock);
    const bool installed = !lifecycle::g_transitionActive && lifecycle::all_installed()
                           && g_trackCalls && lifecycle::all_accepting()
                           && g_contentFetch != nullptr
                           && gate::is_disabled(lifecycle::g_gateOwnership);
    ReleaseSRWLockShared(&g_lifecycleLock);
    return installed;
}

/** @return True while any hook, gate, transition or live call still needs cleanup. */
bool has_ownership() noexcept {
    AcquireSRWLockShared(&g_lifecycleLock);
    const bool owned = lifecycle::any_installed() || g_trackCalls || lifecycle::g_transitionActive
                       || lifecycle::g_gateOwnership.address != nullptr
                       || coordinator::active_calls() != 0;
    ReleaseSRWLockShared(&g_lifecycleLock);
    return owned;
}

} // namespace sunrise::client::hooks::network::content_config
