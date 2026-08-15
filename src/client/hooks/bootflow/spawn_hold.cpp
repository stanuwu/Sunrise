#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/runtime.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The player spawn gate. Anchored on the load of the encrypted manager global, then run on
 * through the stack-cookie store because the wildcarded frame size leaves the head too short.
 */
constexpr std::string_view kSpawnGateSignatureText =
    "40 53 57 41 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 8B D9 "
    "40 B7 01";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSpawnGateSignature =
    signature<signature_length(kSpawnGateSignatureText)>(kSpawnGateSignatureText);

/** Answer that holds the spawn for this tick. The gate is polled, so a refusal only delays it. */
constexpr bool kHeld = false;

using SpawnGate = bool(__fastcall*)(std::int32_t) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<SpawnGate> g_original{nullptr};

/**
 * Puts the spawn after the world-transition fade is armed.
 * A release on a channel that is not up does nothing, so a spawn during the load leaves the
 * screen black. The client's own predicate reads a host field this destination never fills.
 * @param datum Borrowed player datum handle; the answer does not depend on it.
 * @return The native answer, or held while a destination load is still running.
 */
__declspec(noinline) bool __fastcall spawn_gate(std::int32_t datum) noexcept {
    const SpawnGate original = g_original.load(std::memory_order_acquire);
    const bool allowed = original != nullptr && original(datum);
    observe_world_step();
    const state::activity::WorldPhase phase = state::activity::world_phase();
    const bool transitioning = phase == state::activity::WorldPhase::transitioning;
    // Zero unless a load is running.
    const std::uint64_t age = state::activity::world_transition_age();
    const core::settings::client::Settings& client = core::settings::get().client;
    const bool gaveUp = age >= client.spawnHoldMs;
    const bool loading = transitioning && !gaveUp && client.holdSpawn;
    // Release only on arrival. The step-37 exit re-arms the fade unless one is already up, and
    // nothing polls this gate after the spawn, so an early release leaves a fade nobody clears.
    if (phase == state::activity::WorldPhase::arrived) {
        release_world_fade();
    }
    return allowed && loading ? kHeld : allowed;
}

} // namespace

/** Attaches the spawn hold. */
bool install_spawn_hold() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kSpawnGateSignature, "player_spawn_gate");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=spawn_hold result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&spawn_gate)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=spawn_hold result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<SpawnGate>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=spawn_hold result=ok");
    return true;
}

/** Detaches the spawn hold. */
void uninstall_spawn_hold() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
