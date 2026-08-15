#include "queuez_hook_lifecycle.h"

#include "internal.h"

namespace sunrise::client::hooks::queuez {

/** Attaches the queuez fixes. */
bool install() noexcept {
    const bool subscription = install_family0_subscription();
    const bool guard = install_null_payload_guard();
    return subscription && guard;
}

/** Detaches every queuez fix, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_null_payload_guard();
    uninstall_family0_subscription();
}

/** @return True while at least one queuez fix is attached. */
bool is_installed() noexcept {
    return family0_subscription_installed() || null_payload_guard_installed();
}

} // namespace sunrise::client::hooks::queuez
