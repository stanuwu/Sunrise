#include "../../core/logging/log.h"
#include "../content/investment/worker.h"
#include "../hooks/assert_handler/assert_handler_lifecycle.h"
#include "../hooks/banner/banner_hook_lifecycle.h"
#include "../hooks/bitmap/bitmap_hook_lifecycle.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/config_getter/config_getter_lifecycle.h"
#include "../hooks/cursor/runtime.h"
#include "../hooks/graphics/graphics_hook_lifecycle.h"
#include "../hooks/network/runtime.h"
#include "../hooks/polled_input/runtime.h"
#include "../hooks/queuez/queuez_hook_lifecycle.h"
#include "../hooks/retail_log/retail_log_lifecycle.h"
#include "../hooks/teleport/runtime.h"
#include "../targets/game.h"
#include "../targets/steam_targets.h"
#include "../teleport/teleport_settings_store.h"
#include "../ui/runtime/client_ui_module_runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client {

/** Initializes Client-owned process state without installing hooks. */
bool initialize(void* module) noexcept {
    // Loaded before the pages register, so the teleport page draws saved values on its first frame.
    teleport::initialize(module);
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
    // Detached before the other game hooks because each fix is a plain one-site detour with
    // no shared target state to hand back.
    hooks::banner::uninstall();
    hooks::bitmap::uninstall();
    hooks::bootflow::uninstall();
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
    teleport::shutdown();
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=shutdown result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
