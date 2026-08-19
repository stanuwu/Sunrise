#include "gameplay_editor_mode.h"

#include "../../client/hooks/cursor/runtime.h"
#include "../../client/hooks/director/director_handoff.h"
#include "../../client/hooks/polled_input/runtime.h"
#include "../../client/movement/movement_settings_store.h"
#include "../../core/ui/runtime/ui_visibility_runtime.h"
#include "../editor/ui/izanami_panel.h"

namespace sunrise::izanami::runtime::gameplay_editor_mode {
namespace {

client::movement::Settings g_previousMovement{};
bool g_hasPreviousMovement{};
bool g_active{};

} // namespace

/** Enables released Sunrise movement hooks as the first real-game Forge navigation backend. */
ActivationResult enter() noexcept {
    if (!g_active) {
        g_previousMovement = client::movement::get();
        g_hasPreviousMovement = true;
    }

    client::movement::Settings movement = client::movement::get();
    movement.noclipEnabled = true;
    movement.flyEnabled = true;
    if (movement.flySpeed < 25.0F) {
        movement.flySpeed = 25.0F;
    }

    ActivationResult result{};
    result.navigationEnabled = client::movement::publish(movement);
    result.uiHidden = core::ui::runtime::set_visible(false);
    (void)editor::ui::set_standalone_visible(false);
    client::hooks::cursor::apply_visibility(false);
    client::hooks::polled_input::apply_visibility(false);
    g_active = result.navigationEnabled;
    return result;
}

/** Requests Destiny's own Director UI as the next activity-load trigger. */
NativeDirectorHandoffResult request_native_director_handoff() noexcept {
    const bool uiHidden = core::ui::runtime::set_visible(false);
    (void)editor::ui::set_standalone_visible(false);
    client::hooks::cursor::apply_visibility(false);
    client::hooks::polled_input::apply_visibility(false);
    const client::hooks::director::HandoffResult handoff =
        client::hooks::director::request_open_destinations();
    return {.requested = handoff.requested,
            .usedDestinationsTab = handoff.usedDestinationsTab,
            .usedDirector = handoff.usedDirector,
            .usedFallback = handoff.usedFallback,
            .accountBindingsConfigured = handoff.accountBindingsConfigured,
            .uiHidden = uiHidden};
}

/** Queues Destiny's native activity-session creation step, then yields the screen to the game. */
NativeActivityLaunchResult request_native_activity_launch() noexcept {
    const client::hooks::director::ActivityLaunchResult launch =
        client::hooks::director::request_activity_launch();
    bool uiHidden = false;
    if (launch.requested) {
        uiHidden = core::ui::runtime::set_visible(false);
        (void)editor::ui::set_standalone_visible(false);
        client::hooks::cursor::apply_visibility(false);
        client::hooks::polled_input::apply_visibility(false);
    }
    return {.requested = launch.requested,
            .targetResolved = launch.targetResolved,
            .inOrbit = launch.inOrbit,
            .uiHidden = uiHidden};
}

/** Restores movement settings captured before Izanami entered anchored editor mode. */
void leave() noexcept {
    client::hooks::director::cancel();
    if (!g_active) {
        return;
    }
    if (g_hasPreviousMovement) {
        (void)client::movement::publish(g_previousMovement);
    }
    g_previousMovement = {};
    g_hasPreviousMovement = false;
    g_active = false;
}

/** Reports whether anchored editor mode currently owns movement settings. */
bool active() noexcept {
    return g_active;
}

} // namespace sunrise::izanami::runtime::gameplay_editor_mode
