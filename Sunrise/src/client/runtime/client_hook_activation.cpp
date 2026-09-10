#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/ui/busy/busy.h"
#include "../../core/ui/notice/ui_notice_overlay.h"
#include "../../server/bap/runtime.h"
#include "../content/activity/scriptable_catalog_worker.h"
#include "../content/bootstrap/bootstrap_token_publish.h"
#include "../content/investment/worker.h"
#include "../executable/image.h"
#include "../hooks/assert_handler/assert_handler_lifecycle.h"
#include "../hooks/async_io/async_io_lifetime_guard.h"
#include "../hooks/bitmap/bitmap_hook_lifecycle.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/cine_auth_probe/cine_auth_probe.h"
#include "../hooks/cine_probe/cine_probe.h"
#include "../hooks/config_getter/config_getter_lifecycle.h"
#include "../hooks/cursor/runtime.h"
#include "../hooks/graphics/graphics_hook_lifecycle.h"
#include "../hooks/hitch_probe/hitch_probe.h"
#include "../hooks/inactivity/inactivity_override.h"
#include "../hooks/infinite_ammo/infinite_ammo.h"
#include "../hooks/membership_probe/membership_probe.h"
#include "../hooks/network/investment/investment_derived_rebuild.h"
#include "../hooks/network/runtime.h"
#include "../hooks/noclip/runtime.h"
#include "../hooks/package_trust/package_trust_bypass.h"
#include "../hooks/polled_input/runtime.h"
#include "../hooks/queuez/queuez_hook_lifecycle.h"
#include "../hooks/retail_log/retail_log_lifecycle.h"
#include "../hooks/sense_chain_guard/sense_chain_guard.h"
#include "../hooks/stall_probe/stall_probe.h"
#include "../hooks/teleport/runtime.h"
#include "../hooks/turn_back/turn_back.h"
#include "../hooks/world_objects/world_object_registry.h"
#include "../patterns/registry.h"
#include "../player/controlled_object.h"
#include "../targets/game.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::runtime {

SRWLOCK g_lock{SRWLOCK_INIT};
StageState g_mainStage{StageState::pending};
StageState g_graphicsStage{StageState::pending};
StageState g_platformStage{StageState::pending};
HMODULE g_platformModule{};

namespace {

/** Main-image executable ranges remain valid while the process is loaded. */
struct GameImageRanges {
    executable::ExecutableImage executable;
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
};

/**
 * Inspects the main image and maps its executable sections to scanner ranges.
 * @param output Receives the inspected image and matching scanner ranges.
 * @return True when the main PE image has at least one valid executable range.
 */
[[nodiscard]] bool inspect_game_image(GameImageRanges& output) noexcept {
    output = {};
    if (!executable::inspect_main_module(output.executable)) {
        return false;
    }
    for (std::size_t index = 0; index < output.executable.count; ++index) {
        output.ranges[index] = patterns::ImageRange{output.executable.sections[index]};
    }
    return true;
}

/** @param image Inspected main image. @return Populated executable scanner ranges. */
[[nodiscard]] std::span<patterns::ImageRange> ranges(GameImageRanges& image) noexcept {
    return std::span(image.ranges.data(), image.executable.count);
}

/** Reports which resolve stage rejected the sweep, naming a missed signature. */
void report_resolve_failure() noexcept {
    const auto failure = targets::game::resolution::last_failure();
    if (failure == targets::game::resolution::Failure::networkDerive) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=game_targets reason=network_derive result=fail");
        return;
    }
    if (failure == targets::game::resolution::Failure::contentDerive) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=game_targets reason=content_derive result=fail");
        return;
    }
    const std::string_view name = targets::game::resolution::last_failed_signature();
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activate stage=game_targets reason=signature name=%.*s "
                                      "result=fail",
                                      static_cast<int>(name.size()),
                                      name.data());
    if (written <= 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=game_targets reason=signature result=fail");
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(
        core::log::Channel::client, core::log::Level::error, std::string_view(line.data(), length));
}

/** Clears both main-image target groups while no game hook owns their entries. */
void clear_game_targets() noexcept {
    targets::game::content::clear();
    targets::game::network::clear();
}

/**
 * Resolves both main-image target groups from one inspection, then installs game hooks.
 * @return True when every required main-image target and game hook is ready.
 */
[[nodiscard]] bool activate_required_main_locked() noexcept {
    GameImageRanges gameImage;
    if (!inspect_game_image(gameImage)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=game_image result=fail");
        clear_game_targets();
        return false;
    }
    const std::span<patterns::ImageRange> imageRanges = ranges(gameImage);
    if (!targets::game::resolution::resolve(imageRanges)) {
        report_resolve_failure();
        return false;
    }
    // Steam initialization installs package trust before base-package registration. Keep this
    // idempotent check beside the other main-image hooks so activation also verifies ownership.
    if (!hooks::package_trust::install()) {
        clear_game_targets();
        return false;
    }
    // The SignOn config blob carries this token. It must reach State before any hook owns the
    // resolved targets: extraction cannot recover from a missing bootstrap token.
    if (!content::bootstrap::publish_token()) {
        (void)hooks::package_trust::uninstall();
        clear_game_targets();
        return false;
    }
    if (!hooks::network::install_game()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=game_network result=fail");
        if (!hooks::network::has_game_ownership()) {
            (void)hooks::package_trust::uninstall();
            clear_game_targets();
        }
        return false;
    }

    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=activate stage=game_network result=ok");
    const bool packageKeys = targets::game::packages::is_resolved();
    core::log::write(core::log::Channel::client,
                     packageKeys ? core::log::Level::info : core::log::Level::warn,
                     packageKeys ? "ev=activate stage=package_keys result=ok"
                                 : "ev=activate stage=package_keys result=fail");
    // Diagnostic capture reports its own outcome and never demotes this stage.
    (void)hooks::retail_log::install();
    (void)hooks::assert_handler::install();
    // Read-only. At a hitch it dumps every in-flight job record from the watchdog snapshot,
    // which names the job and thread the in-world freeze blocks on.
    (void)hooks::hitch_probe::install();
    // Read-only. Some freezes silence the watchdog too; this watcher dumps every thread's rip
    // and stack from its own thread when the game stops calling the pump.
    (void)hooks::stall_probe::install();
    // A sense-record chain that stops terminating after a slice-set teardown holds the whole
    // frame graph. The guard logs the runaway chain and skips its walk for that tick.
    (void)hooks::sense_chain_guard::install();
    // The stock async-I/O wrapper reloads its singleton after pumping it and can observe the
    // legitimate teardown/recreate null window. This optional guard keeps the owner it pumped.
    (void)hooks::async_io::install();
    (void)hooks::config_getter::install();
    // Boot-step fixes scan for their own single-site targets; each reports its own outcome.
    (void)hooks::bootflow::install();
    // Resolve the shared player accessor independently of either hook.
    (void)player::controlled_object::resolve();
    // The teleport hooks attach whether or not the feature is on, so the interface can enable it
    // without a restart. Both replacements return immediately while nothing is requested.
    (void)hooks::teleport::install();
    // Optional and fail-closed: a changed game build must not make initialization fail.
    (void)hooks::turn_back::install();
    // Noclip owns its Havok-step target, so a patch-specific miss cannot disable teleport.
    (void)hooks::noclip::install();
    // Attaches whether or not the feature is on, so the interface can enable it without a restart.
    (void)hooks::infinite_ammo::install();
    // Resolves the activity config getter here; the hold itself runs on the frame tick.
    (void)hooks::inactivity::install();
    (void)hooks::queuez::install();
    // The bitmap reference guard puts the none sentinel in place of a reference outside tag
    // space. Without it the widget's stored-reference reader faults.
    (void)hooks::bitmap::install();
    // Read-only. It reports the status word the activity msg 12 handler writes. That word is the
    // one thing separating "the client never saw our membership body" from "it saw it and the
    // world container still did not bind".
    (void)hooks::membership_probe::install();
    // Read-only. While the prologue-filler boot task runs, it logs once per second which
    // cinematic readiness stage is false, the thing the task's five-second timeout hides.
    (void)hooks::cine_probe::install();
    // Read-only. Logs the type-6 cinematic Auth chain: the armed gate, the body copy, each
    // silent start gate with the compared values, and the start outcome.
    (void)hooks::cine_auth_probe::install();
    // Retains the native handle for package placements without publishing unnamed map objects.
    (void)hooks::world_objects::install();
    // The server reports investment publications through these and never calls the Client.
    if (!server::bap::register_client_investment_consumers(
            &hooks::network::investment::notify_investment_publication,
            &content::investment::worker::request_slice)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activation stage=investment_consumers result=fail");
    }
    content::investment::worker::activate();
    content::activity::scriptables::activate();
    return true;
}

} // namespace

} // namespace sunrise::client::runtime

namespace sunrise::client {

/** Resolves main-image targets and installs required game hooks once. */
bool activate_main_once() noexcept {
    AcquireSRWLockExclusive(&runtime::g_lock);
    if (runtime::g_mainStage != runtime::StageState::pending) {
        const bool active = runtime::g_mainStage == runtime::StageState::active;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return active;
    }
    // The image sweep dominates this call, so the pair of debug markers around it is what a
    // boot-time measurement reads. Both are diagnostic and stay off at the usual levels.
    core::log::write(
        core::log::Channel::client, core::log::Level::debug, "ev=activate stage=main phase=begin");
    // The sweep stalls whichever thread calls it, so the overlay says what is happening. It
    // only reaches the screen once the presentation hooks are installed.
    core::ui::busy::begin(core::ui::busy::Task::initialization);
    // Started after the overlay is up, because begin blocks for up to half a second waiting on
    // presents. That wait belongs to the overlay, not to the work being measured.
    const std::uint64_t startedTick = GetTickCount64();
    const bool active = runtime::activate_required_main_locked();
    core::log::write_elapsed(core::log::Channel::client,
                             "ev=activate stage=main phase=complete",
                             startedTick,
                             active ? "ok" : "fail");
    core::ui::busy::end(core::ui::busy::Task::initialization);
    if (!active) {
        // A failed sweep latches too: repeating it stalls the frame loop for nothing.
        runtime::g_mainStage = runtime::StageState::failed;
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=main result=fail");
        // The boot cannot reach orbit after this, so the user is told rather than left waiting.
        core::ui::notice::raise("Sunrise could not attach to the game. The boot will not finish.");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }

    runtime::g_mainStage = runtime::StageState::active;
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=activate stage=main result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

/** Installs the presentation hooks once, independently of the game image sweep. */
bool activate_graphics_once() noexcept {
    AcquireSRWLockExclusive(&runtime::g_lock);
    if (runtime::g_graphicsStage != runtime::StageState::pending) {
        const bool active = runtime::g_graphicsStage == runtime::StageState::active;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return active;
    }
    if (!hooks::graphics::install()) {
        runtime::g_graphicsStage = runtime::StageState::failed;
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=graphics_hooks result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }

    runtime::g_graphicsStage = runtime::StageState::active;
    // The cursor guards only matter once the interface can be shown. A miss must not demote
    // presentation readiness, so it is logged and not propagated.
    (void)hooks::cursor::install();
    // The game reads its action keys by scanning GetKeyState every frame, which no window
    // procedure sees, so the polled guards carry the same terms as the cursor guards.
    (void)hooks::polled_input::install();
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=activate stage=graphics result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
