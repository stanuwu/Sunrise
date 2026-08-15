/**
 * The teleport module's interface. Every control writes the configuration straight to disk, so a
 * change made here survives the next launch without a settings edit.
 */

#include "teleport_panel.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../teleport/teleport_settings_store.h"

namespace sunrise::client::ui::teleport {
namespace {

/** Lowest and highest virtual keys the picker scans. Zero is not a key. */
constexpr int kFirstVirtualKey = 1;
constexpr int kLastVirtualKey = 254;
/** Mouse buttons are skipped so a click on the picker cannot bind itself. */
constexpr int kLastMouseKey = 6;
/** Longest key name Windows returns, plus the null. */
constexpr std::size_t kKeyNameCapacity = 64;

bool g_capturing{};

/**
 * Names one virtual key for display.
 * @param virtualKey Key to name, or zero for no binding.
 * @param output Receives the name.
 */
void key_name(std::uint32_t virtualKey, std::array<char, kKeyNameCapacity>& output) noexcept {
    if (virtualKey == client::teleport::kNoKey) {
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
        picked = client::teleport::kNoKey;
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

} // namespace

/** Draws the teleport module inside the active Core UI frame. */
void draw() noexcept {
    client::teleport::Settings settings = client::teleport::get();
    bool changed = false;

    ImGui::TextUnformatted("Teleport");
    ImGui::Separator();
    ImGui::TextWrapped("Teleports you forward in the facing direction. "
                       "Cancels vertical momentum.");
    ImGui::Spacing();

    changed = core::ui::components::toggle::control("Enabled", settings.enabled) || changed;

    ImGui::Spacing();
    // One label column and one control column, so the slider and the key button share both edges.
    const float labelWidth =
        ImGui::CalcTextSize("Distance").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Distance");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    float distance = settings.distance;
    if (ImGui::SliderFloat("##distance",
                           &distance,
                           client::teleport::kMinimumDistance,
                           client::teleport::kMaximumDistance,
                           "%.0f units")) {
        settings.distance = distance;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Key");
    ImGui::SameLine(labelWidth);
    if (g_capturing) {
        if (ImGui::Button("...", ImVec2(controlWidth, 0.0F))) {
            g_capturing = false;
        }
        std::uint32_t picked = client::teleport::kNoKey;
        if (capture_key(picked)) {
            settings.virtualKey = picked;
            g_capturing = false;
            changed = true;
        }
    } else {
        std::array<char, kKeyNameCapacity> name{};
        key_name(settings.virtualKey, name);
        if (ImGui::Button(name.data(), ImVec2(controlWidth, 0.0F))) {
            g_capturing = true;
        }
    }

    if (changed && !client::teleport::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::teleport
