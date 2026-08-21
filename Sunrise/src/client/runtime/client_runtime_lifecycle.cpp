#include "../../core/logging/log.h"
#include "../content/investment/worker.h"
#include "../hooks/assert_handler/assert_handler_lifecycle.h"
#include "../hooks/bitmap/bitmap_hook_lifecycle.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/config_getter/config_getter_lifecycle.h"
#include "../hooks/cursor/runtime.h"
#include "../hooks/graphics/graphics_hook_lifecycle.h"
#include "../hooks/godmode/godmode.h"
#include "../hooks/inactivity/inactivity_override.h"
#include "../hooks/infinite_ammo/infinite_ammo.h"
#include "../hooks/network/runtime.h"
#include "../hooks/noclip/runtime.h"
#include "../hooks/no_turnback/no_turnback.h"
#include "../hooks/package_trust/package_trust_bypass.h"
#include "../hooks/polled_input/runtime.h"
#include "../hooks/queuez/queuez_hook_lifecycle.h"
#include "../hooks/retail_log/retail_log_lifecycle.h"
#include "../hooks/teleport/runtime.h"
#include "../inactivity/inactivity_settings_store.h"
#include "../movement/movement_settings_store.h"
#include "../player/player_settings_store.h"
#include "../targets/game.h"
#include "../targets/steam_targets.h"
#include "../ui/runtime/client_ui_module_runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client {

/** Initializes Client-owned process state without installing hooks. */
bool initialize(void* module) noexcept {
    // Loaded before the pages register, so each page draws saved values on its first frame.
    movement::initialize(module);
    player::initialize(module);
    inactivity::initialize(module);
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
    // Detached after presentation, so no later frame can apply the cursor policy.
    hooks::cursor::uninstall();
    hooks::polled_input::uninstall();
    if (!hooks::network::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=network_hooks result=fail");
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
    hooks::bitmap::uninstall();
    hooks::bootflow::uninstall();
    hooks::infinite_ammo::uninstall();
    hooks::godmode::uninstall();
    hooks::no_turnback::uninstall();
    hooks::inactivity::uninstall();
    hooks::noclip::uninstall();
    hooks::teleport::uninstall();
    hooks::queuez::uninstall();
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
    content::investment::worker::reset();
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
    inactivity::shutdown();
    player::shutdown();
    movement::shutdown();
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=shutdown result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
