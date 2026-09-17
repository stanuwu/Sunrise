#include "bootflow_hook_lifecycle.h"

#include <atomic>

#include "internal.h"
#include "spawn/slice_set_sample.h"

namespace sunrise::client::hooks::bootflow {
namespace {

std::atomic_bool g_installed{false};

/** One boot-step fix that attaches a detour, in the order the group installs them. */
struct Fix {
    StageResult (*stage)(hooking::detour::Spec&) noexcept;
    void (*publish)(const hooking::detour::Handle&) noexcept;
};

/**
 * Every fix that attaches a detour. `world_step` and the slice-set sample are absent: they only
 * find addresses to call, so they open no transaction and cost the group nothing.
 */
constexpr std::array kFixes{
    Fix{&stage_character_select_hold, &publish_character_select_hold},
    Fix{&stage_orbit_slice_set, &publish_orbit_slice_set},
    Fix{&stage_orbit_handoff, &publish_orbit_handoff},
};

/** Marks a fix that staged nothing, so no handle is ever published to it. */
constexpr std::size_t kNotStaged = kFixes.size();

/** One fix's place in the batch, and what it already was before staging. */
struct Placement {
    std::size_t slot{kNotStaged};
    StageResult result{StageResult::unavailable};
};

} // namespace

/**
 * Finds the boot-step accessor and the slice-set sample targets, and attaches the lifetime probe.
 * @return True when all three are ready.
 */
bool install() noexcept {
    const bool worldStep = install_world_step();
    const bool sliceSet = spawn::install_targets();
    const bool probe = install_lifetime_gate_probe();
    g_installed.store(worldStep || sliceSet || probe, std::memory_order_release);
    return worldStep && sliceSet && probe;
}

/** Detaches the probe and clears both accessors, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_lifetime_gate_probe();
    spawn::uninstall_targets();
    uninstall_world_step();
    uninstall_orbit_handoff();
    uninstall_orbit_slice_set();
    uninstall_character_select_hold();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while at least one accessor or the probe is installed. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::bootflow
