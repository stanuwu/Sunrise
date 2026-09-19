/** The player module's interface. Every control saves at once, so a change survives a restart. */

#include "player_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {

/** Draws the player module inside the active Core UI frame. */
void draw() noexcept {
    namespace toggle = core::ui::components::toggle;
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep weapon ammunition full.");
    ImGui::Spacing();
    bool changed =
        toggle::control("Infinite Reserves##infinite_ammo", settings.infiniteAmmoEnabled);
    changed = toggle::control("Infinite Magazine##infinite_magazine",
                              settings.infiniteMagazineEnabled)
              || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Anti AFK");
    ImGui::Separator();
    ImGui::TextWrapped("Disable AFK timeouts from activities kicking to orbit and the title "
                       "screen.");
    ImGui::Spacing();
    changed = toggle::control("Enabled##anti_afk", settings.antiAfkEnabled) || changed;

    if (changed) {
        (void)client::player::publish(settings);
    }
}

} // namespace sunrise::client::ui::player
