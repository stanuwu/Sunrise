#pragma once
#include <imgui.h>

namespace sunrise::client::ui::mission_launch::detail {
/** Shared contrast for every Activity Launcher page; restores the surrounding overlay palette on
 * exit. */
struct Style final {
    Style() noexcept {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.96F, 0.97F, 0.99F, 1});
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, {0.68F, 0.74F, 0.82F, 1});
        ImGui::PushStyleColor(ImGuiCol_Border, {0.40F, 0.49F, 0.61F, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.13F, 0.18F, 0.24F, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.22F, 0.31F, 0.42F, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.28F, 0.40F, 0.54F, 1});
        ImGui::PushStyleColor(ImGuiCol_Button, {0.17F, 0.23F, 0.31F, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.27F, 0.36F, 0.47F, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.34F, 0.44F, 0.57F, 1});
        ImGui::PushStyleColor(ImGuiCol_Header, {0.20F, 0.33F, 0.48F, 1});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.29F, 0.42F, 0.56F, 1});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.35F, 0.49F, 0.65F, 1});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.07F, 0.09F, 0.13F, 1});
        ImGui::PushStyleColor(ImGuiCol_NavCursor, {1.0F, 0.64F, 0.30F, 1});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 0.60F);
    }
    ~Style() noexcept {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(14);
    }
    Style(const Style&) = delete;
    Style& operator=(const Style&) = delete;
};

inline void heading(const char* text) noexcept {
    // PushFont takes an unscaled base size; passing GetFontSize would double-apply DPI.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.375F);
    ImGui::TextWrapped("%s", text);
    ImGui::PopFont();
}

[[nodiscard]] inline bool launch_button(bool busy, bool disabled, float height) noexcept {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          disabled ? ImVec4{0.19F, 0.24F, 0.30F, 1}
                                   : ImVec4{0.97F, 0.51F, 0.25F, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1.0F, 0.64F, 0.37F, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.86F, 0.40F, 0.17F, 1});
    ImGui::PushStyleColor(ImGuiCol_Text,
                          disabled ? ImVec4{0.91F, 0.94F, 0.98F, 1}
                                   : ImVec4{0.035F, 0.045F, 0.065F, 1});
    ImGui::BeginDisabled(disabled);
    const bool clicked = ImGui::Button(busy ? "Launching..." : "Launch activity", {-1.0F, height});
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    return clicked;
}
} // namespace sunrise::client::ui::mission_launch::detail
