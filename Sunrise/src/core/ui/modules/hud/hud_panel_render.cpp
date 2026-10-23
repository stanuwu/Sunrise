/** The HUD page. One switch per overlay, in the order the overlays stack on screen. */

#include <cstddef>
#include <imgui.h>

#include "../../components/section/ui_section_component.h"
#include "../../components/toggle/ui_toggle_component.h"
#include "../../hud/overlay.h"
#include "internal.h"

namespace sunrise::core::ui::modules::hud::internal {

/** Draws the HUD page inside the active Core UI frame. */
void draw() noexcept {
    ImGui::TextWrapped("Overlays draw in the top corners while the game runs, with or without "
                       "this menu open. Mission Variables takes the top-right corner.");
    ImGui::Spacing();

    for (std::size_t index = 0; index < static_cast<std::size_t>(ui::hud::Overlay::count);
         ++index) {
        const auto overlay = static_cast<ui::hud::Overlay>(index);
        bool on = ui::hud::enabled(overlay);
        // The label is unique per overlay, so it carries the control's identity on its own.
        if (components::toggle::control(ui::hud::display_name(overlay), on)) {
            ui::hud::set_enabled(overlay, on);
        }
    }

    ImGui::Spacing();
    components::section::header("Current status lines",
                                "Each line of the current status overlay, on its own.");
    ImGui::Spacing();
    for (std::size_t index = 0; index < static_cast<std::size_t>(ui::hud::StatusLine::count);
         ++index) {
        const auto line = static_cast<ui::hud::StatusLine>(index);
        bool on = ui::hud::enabled(line);
        if (components::toggle::control(ui::hud::display_name(line), on)) {
            ui::hud::set_enabled(line, on);
        }
    }
}

} // namespace sunrise::core::ui::modules::hud::internal
