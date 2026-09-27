#include "ui_hud_startup_hint_overlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <imgui.h>
#include <string_view>

#include "../../../settings/parser.h"
#include "../../runtime/ui_visibility_runtime.h"

namespace sunrise::core::ui::hud::overlays::startup_hint {
namespace {

/** 2 seconds let the game settle before the hint asks for attention. */
constexpr float kDelaySeconds = 2.0F;
/** 30 seconds is long enough to notice the hint and short enough to clear itself. */
constexpr float kVisibleSeconds = 30.0F;
/** Half a second to fade in or out, slower than the menus so the hint stays subtle. */
constexpr float kFadeSeconds = 0.5F;
/** A hidden hint has finished fading out and draws nothing. */
constexpr float kHiddenOpacity = 0.0F;
/** A shown hint has finished fading in. */
constexpr float kShownOpacity = 1.0F;
/** Longest supported toggle key name, plus the null. */
constexpr std::size_t kKeyNameCapacity = 16;

/** Seconds since the first HUD frame, counted until the hint is dismissed. */
float g_elapsedSeconds = 0.0F;
/** Set once the surface opened or the stay ran out. The hint never returns in this run. */
bool g_dismissed = false;
/** The hint's own fade, so it needs no shared animation key. */
float g_opacity = kHiddenOpacity;

/**
 * Names the configured toggle key in upper case, so it stands out from the muted text around it.
 * The key always comes from the parser's table, because the settings reject any other one.
 * @return Null-terminated key name.
 */
[[nodiscard]] std::array<char, kKeyNameCapacity> key_label() noexcept {
    std::array<char, kKeyNameCapacity> label{};
    const std::string_view name =
        settings::parser::Parser::ui_toggle_key_name(runtime::snapshot().toggleVirtualKey);
    const std::size_t length = (std::min)(name.size(), label.size() - 1);
    for (std::size_t index = 0; index < length; ++index) {
        label[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[index])));
    }
    return label;
}

} // namespace

/** Advances the hint's one-time lifetime by this frame. */
float progress() noexcept {
    const float deltaSeconds = ImGui::GetIO().DeltaTime;
    if (!g_dismissed) {
        g_elapsedSeconds += deltaSeconds;
        // An open surface has taught the key, and a hint that came back later would nag.
        if (runtime::snapshot().visible || g_elapsedSeconds >= kDelaySeconds + kVisibleSeconds) {
            g_dismissed = true;
        }
    }
    const bool shown = !g_dismissed && g_elapsedSeconds >= kDelaySeconds;
    const float step = deltaSeconds / kFadeSeconds;
    g_opacity = shown ? (std::min)(g_opacity + step, kShownOpacity)
                      : (std::max)(g_opacity - step, kHiddenOpacity);
    return g_opacity;
}

/** Draws the startup hint inside the overlay window the stack has already started. */
void draw() noexcept {
    const std::array<char, kKeyNameCapacity> key = key_label();
    // The sentence is muted like the card's version line; the key is white like its wordmark.
    ImGui::TextDisabled("Press");
    ImGui::SameLine();
    ImGui::TextUnformatted(key.data());
    ImGui::SameLine();
    ImGui::TextDisabled("to open the Sunrise overlay");
}

} // namespace sunrise::core::ui::hud::overlays::startup_hint
