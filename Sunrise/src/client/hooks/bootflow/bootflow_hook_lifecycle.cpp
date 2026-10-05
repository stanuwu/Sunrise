#include "bootflow_hook_lifecycle.h"

#include <atomic>

#include "internal.h"
#include "spawn/slice_set_sample.h"

namespace sunrise::client::hooks::bootflow {
namespace {

std::atomic_bool g_installed{false};

} // namespace

/**
 * Finds the boot-step accessor and the slice-set sample targets.
 * Nothing is detoured: the boot steps run as shipped and the host answers them.
 * @return True when both targets were found.
 */
bool install() noexcept {
    const bool worldStep = install_world_step();
    const bool sliceSet = spawn::install_targets();
    const bool probe = install_lifetime_gate_probe();
    const bool characterSelectHold = install_character_select_hold();
    g_installed.store(worldStep || sliceSet || probe || characterSelectHold,
                      std::memory_order_release);
    return worldStep && sliceSet && probe && characterSelectHold;
}

/** Clears both accessors, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_character_select_hold();
    uninstall_lifetime_gate_probe();
    spawn::uninstall_targets();
    uninstall_world_step();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while at least one accessor is found. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::bootflow
