/**
 * The HUD overlay stack. Overlays are fixed windows in the top-left corner: each one draws its
 * own content, and the stack places it under the one before it.
 */

#include <array>
#include <cstddef>
#include <imgui.h>

#include "../scaling/dpi/ui_dpi_scaling.h"
#include "overlay.h"
#include "overlays/ui_hud_logo_overlay.h"
#include "overlays/ui_hud_mission_script_overlay.h"
#include "overlays/ui_hud_sensor_events_overlay.h"
#include "overlays/ui_hud_session_overlay.h"
#include "overlays/ui_hud_startup_hint_overlay.h"
#include "overlays/ui_hud_status_overlay.h"
#include "store/hud_settings_store.h"

namespace sunrise::core::ui::hud {
namespace {

/** One overlay's identity, frame entry and starting switch state. */
struct Entry {
    const char* displayName;
    /** Key the switch file stores this overlay under. It must outlive every table change. */
    const char* storageKey;
    const char* windowId;
    void (*draw)() noexcept;
    bool startsOn;
};

/** 24 pixels keep the stack clear of the viewport corner, as the other overlays do. */
constexpr float kViewportMargin = 24.0F;
/** 8 pixels separate two stacked overlays. */
constexpr float kOverlayGap = 8.0F;
/** Overlays carry no decoration, take no input, and are never saved. */
constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
    | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
/** Overlay count, taken from the enum so the table and the switches cannot disagree. */
constexpr std::size_t kOverlayCount = static_cast<std::size_t>(Overlay::count);
/** Status-line count, taken from the enum for the same reason. */
constexpr std::size_t kStatusLineCount = static_cast<std::size_t>(StatusLine::count);
/** Every switch the file carries: one per overlay, then one per status line. */
constexpr std::size_t kSwitchCount = kOverlayCount + kStatusLineCount;

/** Every overlay, in Overlay order. The menu lists them and the corner stacks them in it. */
constexpr std::array<Entry, kOverlayCount> kOverlays{
    Entry{"Sunrise Card", "sunrise_card", "##sunrise_hud_card", &overlays::logo::draw, true},
    Entry{"Startup Hint",
          "startup_hint",
          "##sunrise_hud_startup_hint",
          &overlays::startup_hint::draw,
          true},
    // Diagnostic overlays start off because an ordinary run does not need them on screen.
    Entry{
        "Current Status", "current_status", "##sunrise_hud_status", &overlays::status::draw, false},
    Entry{"Session", "session", "##sunrise_hud_session", &overlays::session::draw, false},
    Entry{"Client Activity Messages",
          "sensor_events",
          "##sunrise_hud_sensor_events",
          &overlays::sensor_events::draw,
          false},
    Entry{"Mission Script",
          "mission_script",
          "##sunrise_hud_mission_script",
          &overlays::mission_script::draw,
          false},
};

/** One status line's identity and starting switch state. */
struct LineEntry {
    const char* displayName;
    /** Key the switch file stores this line under. It must outlive every table change. */
    const char* storageKey;
    bool startsOn;
};

/** Every status line, in StatusLine order. The overlay draws them in it. */
constexpr std::array<LineEntry, kStatusLineCount> kStatusLines{
    LineEntry{"Activity", "status_activity", true},
    LineEntry{"Bubble", "status_bubble", true},
    LineEntry{"Slice set", "status_slice_set", true},
    LineEntry{"Closest spawn", "status_closest_spawn", true},
};

/** @return The switch state every overlay starts with. */
[[nodiscard]] constexpr std::array<bool, kOverlayCount> starting_state() noexcept {
    std::array<bool, kOverlayCount> state{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] = kOverlays[index].startsOn;
    }
    return state;
}

/** @return The switch state every status line starts with. */
[[nodiscard]] constexpr std::array<bool, kStatusLineCount> starting_line_state() noexcept {
    std::array<bool, kStatusLineCount> state{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        state[index] = kStatusLines[index].startsOn;
    }
    return state;
}

/** Switch state. Only the presentation thread reads or writes it. */
std::array<bool, kOverlayCount> g_enabled{starting_state()};
/** Status-line switch state, on the same thread. */
std::array<bool, kStatusLineCount> g_lineEnabled{starting_line_state()};

/** @param overlay Overlay to check. @return True when it names one of the entries. */
[[nodiscard]] bool in_range(Overlay overlay) noexcept {
    return static_cast<std::size_t>(overlay) < kOverlayCount;
}

/** @param line Status line to check. @return True when it names one of the entries. */
[[nodiscard]] bool in_range(StatusLine line) noexcept {
    return static_cast<std::size_t>(line) < kStatusLineCount;
}

/** @return Every file key paired with the switch state it holds now. */
[[nodiscard]] std::array<store::Switch, kSwitchCount> switch_state() noexcept {
    std::array<store::Switch, kSwitchCount> switches{};
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        switches[index] = {kOverlays[index].storageKey, g_enabled[index]};
    }
    for (std::size_t index = 0; index < kStatusLineCount; ++index) {
        switches[kOverlayCount + index] = {kStatusLines[index].storageKey, g_lineEnabled[index]};
    }
    return switches;
}

/** Writes every switch. One file holds them all, so a partial save would drop the rest. */
void save_switches() noexcept {
    const std::array<store::Switch, kSwitchCount> switches = switch_state();
    // A failed write is reported by the store and never blocks the switch itself.
    (void)store::save(switches);
}

/**
 * Draws one overlay at a fixed position.
 * @param entry Overlay to draw.
 * @param position Top-left corner, in final framebuffer pixels.
 * @return Size the window took, which is known only after its content is submitted.
 */
[[nodiscard]] ImVec2 draw_overlay(const Entry& entry, const ImVec2& position) noexcept {
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    const bool submitContents = ImGui::Begin(entry.windowId, nullptr, kOverlayFlags);
    if (submitContents) {
        entry.draw();
    }
    // Read inside the window, because the size belongs to it and not to the caller's window.
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::End();
    return size;
}

} // namespace

/** Resolves the switch file and applies the saved state. */
void initialize(void* module) noexcept {
    store::initialize(module);
    std::array<store::Switch, kSwitchCount> switches = switch_state();
    store::load(switches);
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        g_enabled[index] = switches[index].on;
    }
    for (std::size_t index = 0; index < kStatusLineCount; ++index) {
        g_lineEnabled[index] = switches[kOverlayCount + index].on;
    }
}

/** Drops the switch file path. The switches keep their values. */
void shutdown() noexcept {
    store::shutdown();
}

/** @return The overlay's menu label. */
const char* display_name(Overlay overlay) noexcept {
    return in_range(overlay) ? kOverlays[static_cast<std::size_t>(overlay)].displayName : "";
}

/** @return True while the overlay draws. */
bool enabled(Overlay overlay) noexcept {
    return in_range(overlay) && g_enabled[static_cast<std::size_t>(overlay)];
}

/** Switches one overlay on or off and saves every switch at once. */
void set_enabled(Overlay overlay, bool on) noexcept {
    if (!in_range(overlay) || g_enabled[static_cast<std::size_t>(overlay)] == on) {
        return;
    }
    g_enabled[static_cast<std::size_t>(overlay)] = on;
    save_switches();
}

/** @return The status line's menu label. */
const char* display_name(StatusLine line) noexcept {
    return in_range(line) ? kStatusLines[static_cast<std::size_t>(line)].displayName : "";
}

/** @return True while the status line draws. */
bool enabled(StatusLine line) noexcept {
    return in_range(line) && g_lineEnabled[static_cast<std::size_t>(line)];
}

/** Switches one status line on or off and saves every switch at once. */
void set_enabled(StatusLine line, bool on) noexcept {
    if (!in_range(line) || g_lineEnabled[static_cast<std::size_t>(line)] == on) {
        return;
    }
    g_lineEnabled[static_cast<std::size_t>(line)] = on;
    save_switches();
}

/** Draws every enabled overlay, stacked down the top-left corner. */
bool draw(bool interfaceEnabled) noexcept {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!interfaceEnabled || viewport == nullptr) {
        return false;
    }
    const float margin = scaling::dpi::pixels(kViewportMargin);
    const float gap = scaling::dpi::pixels(kOverlayGap);
    ImVec2 position{viewport->WorkPos.x + margin, viewport->WorkPos.y + margin};
    // The startup hint sits beside whichever overlay owns the top row, or on the corner when
    // none does. It is drawn after the stack, once that row's width is known, and never moves
    // the overlays stacked below.
    ImVec2 hintPosition = position;
    bool drawn = false;
    for (std::size_t index = 0; index < kOverlayCount; ++index) {
        if (!g_enabled[index] || static_cast<Overlay>(index) == Overlay::startupHint) {
            continue;
        }
        const ImVec2 size = draw_overlay(kOverlays[index], position);
        if (!drawn) {
            hintPosition.x += size.x + gap;
        }
        position.y += size.y + gap;
        drawn = true;
    }
    constexpr auto hintIndex = static_cast<std::size_t>(Overlay::startupHint);
    if (g_enabled[hintIndex]) {
        const float progress = overlays::startup_hint::progress();
        if (progress > 0.0F) {
            // One style alpha fades the hint and everything drawn inside it together.
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
            (void)draw_overlay(kOverlays[hintIndex], hintPosition);
            ImGui::PopStyleVar();
            drawn = true;
        }
    }
    return drawn;
}

} // namespace sunrise::core::ui::hud
