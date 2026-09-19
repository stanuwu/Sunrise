#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../server/bap/runtime.h"
#include "../activity/mission_launch.h"
#include "../content/activity/activity_sdk_generation_worker.h"
#include "../content/activity/scriptable_catalog_worker.h"
#include "../content/investment/worker.h"
#include "../hooks/account_registration/account_registration.h"
#include "../hooks/assert_handler/assert_handler_lifecycle.h"
#include "../hooks/async_io/async_io_lifetime_guard.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/bootflow/bootflow_texture_override.h"
#include "../hooks/cine_probe/cine_probe.h"
#include "../hooks/config_getter/config_getter_lifecycle.h"
#include "../hooks/cursor/runtime.h"
#include "../hooks/graphics/graphics_hook_lifecycle.h"
#include "../hooks/hitch_probe/hitch_probe.h"
#include "../hooks/inactivity/inactivity_override.h"
#include "../hooks/infinite_ammo/infinite_ammo.h"
#include "../hooks/instance_mutex/instance_mutex_release.h"
#include "../hooks/machine_id/machine_id_override.h"
#include "../hooks/network/presence_publication.h"
#include "../hooks/network/runtime.h"
#include "../hooks/noclip/runtime.h"
#include "../hooks/package_trust/package_trust_bypass.h"
#include "../hooks/polled_input/runtime.h"
#include "../hooks/retail_log/retail_log_lifecycle.h"
#include "../hooks/stall_probe/stall_probe.h"
#include "../hooks/teleport/runtime.h"
#include "../hooks/world_objects/world_object_registry.h"
#include "../movement/movement_settings_store.h"
#include "../player/player_settings_store.h"
#include "../targets/game.h"
#include "../targets/steam_targets.h"
#include "../ui/activity/authored_placement_marker.h"
#include "../ui/runtime/client_ui_module_runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client {

/** Initializes Client-owned process state without installing hooks. */
bool initialize(void* module) noexcept {
    content::activity::sdk_generation::initialize(
        module, {core::settings::get().activitySdkGeneration.luaDeclarations});
    // Loaded before the pages register, so each page draws saved values on its first frame.
    movement::initialize(module);
    player::initialize(module);
    ui::activity::authored_placement_marker::initialize(module);
    return ui::runtime::initialize();
}

/** Detaches Client hooks before clearing their resolved target entries. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&runtime::g_lock);
    if (!hooks::graphics::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=graphics_hooks result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    // Stop game-tick producers before clearing their input, world and object accessors.
    if (!hooks::teleport::uninstall()) {
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    // Detached after presentation, so no later frame can apply the cursor policy.
    hooks::cursor::uninstall();
    hooks::polled_input::uninstall();
    if (!hooks::world_objects::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=world_objects result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::presence_publication::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=presence_publication result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::account_registration::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=account_registration result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::machine_id::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=machine_id result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::instance_mutex::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=instance_mutex result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::network::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=network_hooks result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::bootflow::texture_override::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=bootflow_texture result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::package_trust::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=package_trust result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::cine_probe::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=cine_probe result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    // The watcher runs on its own thread, so it stops before the targets it reads are cleared.
    if (!hooks::stall_probe::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=stall_probe result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::hitch_probe::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=hitch_probe result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    activity::mission_launch::uninstall();
    hooks::bootflow::uninstall();
    hooks::infinite_ammo::uninstall();
    hooks::inactivity::uninstall();
    hooks::noclip::uninstall();
    if (!hooks::config_getter::uninstall()) {
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::assert_handler::uninstall()) {
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::retail_log::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=retail_log result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    content::activity::sdk_generation::reset();
    content::activity::scriptables::reset();
    server::bap::unregister_client_investment_slice_consumer();
    content::investment::worker::reset();
    (void)hooks::async_io::uninstall();
    targets::steam::clear();
    if (runtime::g_platformModule != nullptr) {
        if (FreeLibrary(runtime::g_platformModule) == FALSE) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             "ev=shutdown stage=steam_module result=fail");
            ReleaseSRWLockExclusive(&runtime::g_lock);
            return false;
        }
        runtime::g_platformModule = nullptr;
    }
    targets::game::retail_log::clear();
    targets::game::content::clear();
    targets::game::network::clear();
    runtime::g_mainStage = runtime::StageState::pending;
    runtime::g_graphicsStage = runtime::StageState::pending;
    runtime::g_platformStage = runtime::StageState::pending;
    ui::runtime::shutdown();
    // The reverse of the order the stores initialize in.
    ui::activity::authored_placement_marker::shutdown();
    player::shutdown();
    movement::shutdown();
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=shutdown result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
