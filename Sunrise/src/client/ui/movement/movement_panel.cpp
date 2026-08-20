/**
 * The movement module's interface. Every control saves to disk at once, so a change made here
 * survives the next launch with no settings edit.
 */

#include "movement_panel.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <imgui.h>

#include "../../../core/ui/components/label/ui_label_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../movement/movement_settings_store.h"

namespace sunrise::client::ui::movement {
namespace {

namespace label = core::ui::components::label;

/** Lowest and highest virtual keys the picker scans. Zero is not a key. */
constexpr int kFirstVirtualKey = 1;
constexpr int kLastVirtualKey = 254;
/** Mouse buttons are skipped so a click on the picker cannot bind itself. */
constexpr int kLastMouseKey = 6;
/** Longest key name Windows returns, plus the null. */
constexpr std::size_t kKeyNameCapacity = 64;

enum class CaptureTarget {
    none,
    teleport,
    noclip,
    fly,
};

CaptureTarget g_capturing{CaptureTarget::none};

/**
 * Names one virtual key for display.
 * @param virtualKey Key to name, or zero for no binding.
 * @param output Receives the name.
 */
void key_name(std::uint32_t virtualKey, std::array<char, kKeyNameCapacity>& output) noexcept {
    if (virtualKey == client::movement::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, kKeyNameCapacity> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

/**
 * Takes the first key held while the picker is armed.
 * @param picked Receives the key, or zero when Escape clears the binding.
 * @return True when this frame ended the capture.
 */
[[nodiscard]] bool capture_key(std::uint32_t& picked) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        picked = client::movement::kNoKey;
        return true;
    }
    for (int key = kFirstVirtualKey; key <= kLastVirtualKey; ++key) {
        if (key <= kLastMouseKey) {
            continue;
        }
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            picked = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

/**
 * Draws one key picker while keeping capture ownership exclusive.
 * @param id ImGui identity for the button.
 * @param target Binding this picker captures.
 * @param virtualKey Binding value to display and update.
 * @param width Button width.
 * @return True when a new binding was captured.
 */
[[nodiscard]] bool
key_picker(const char* id, CaptureTarget target, std::uint32_t& virtualKey, float width) noexcept {
    ImGui::PushID(id);
    if (g_capturing == target) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturing = CaptureTarget::none;
        }
        ImGui::PopID();
        std::uint32_t picked = client::movement::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturing = CaptureTarget::none;
            return true;
        }
        return false;
    }
    std::array<char, kKeyNameCapacity> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturing = target;
    }
    return false;
}

} // namespace

/** Draws the movement module inside the active Core UI frame. */
void draw() noexcept {
    client::movement::Settings settings = client::movement::get();
    bool changed = false;

    ImGui::TextUnformatted("Teleport");
    ImGui::Separator();
    ImGui::TextWrapped("Teleports you forward in the facing direction. "
                       "Cancels vertical momentum.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##teleport", settings.enabled) || changed;

    ImGui::Spacing();
    // One label column and one control column, so the slider and key buttons share both edges.
    const float labelWidth =
        label::inset() + ImGui::CalcTextSize("Toggle key").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Distance");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    float distance = settings.distance;
    if (ImGui::SliderFloat("##distance",
                           &distance,
                           client::movement::kMinimumDistance,
                           client::movement::kMaximumDistance,
                           "%.0f units")) {
        settings.distance = distance;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Key");
    ImGui::SameLine(labelWidth);
    changed = key_picker("teleport_key", CaptureTarget::teleport, settings.virtualKey, controlWidth)
              || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Noclip");
    ImGui::Separator();
    ImGui::TextWrapped("Disable collision on the horizontal axis.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##noclip", settings.noclipEnabled) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed =
        key_picker("noclip_key", CaptureTarget::noclip, settings.noclipToggleKey, controlWidth)
        || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Fly");
    ImGui::Separator();
    ImGui::TextWrapped("Fly with your movement keys.");
    ImGui::Spacing();

    changed = core::ui::components::toggle::control("Enabled##fly", settings.flyEnabled) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed =
        key_picker("fly_key", CaptureTarget::fly, settings.flyToggleKey, controlWidth) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Type");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    int flyMode = static_cast<int>(settings.flyMode);
    if (ImGui::Combo("##fly_mode", &flyMode, "Velocity\0Coordinate\0")) {
        settings.flyMode = static_cast<client::movement::FlyMode>(flyMode);
        changed = true;
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    float flySpeed = settings.flySpeed;
    if (ImGui::SliderFloat("##fly_speed",
                           &flySpeed,
                           client::movement::kMinimumFlySpeed,
                           client::movement::kMaximumFlySpeed,
                           "%.0f units/s")) {
        settings.flySpeed = flySpeed;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Sword Skate Fix");
    ImGui::Separator();
    ImGui::TextWrapped("Disable sword swings blocking ability usage.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##sword_skate", settings.swordSkateEnabled)
        || changed;

    if (changed && !client::movement::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::movement
