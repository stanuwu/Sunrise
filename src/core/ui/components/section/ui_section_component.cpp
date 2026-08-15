#include "ui_section_component.h"

#include <imgui.h>

#include "../../scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::core::ui::components::section {
namespace {

/** 3 authored pixels keep the rail visible beside short and wrapped headers. */
constexpr float kRailWidth = 3.0F;
/** 7 authored pixels separate the rail from the first title glyph. */
constexpr float kRailSpacing = 7.0F;
/** 2 authored pixels inset the rail ends inside the whole header group. */
constexpr float kRailVerticalInset = 2.0F;
/** 3 authored pixels soften the rail without making a large pill. */
constexpr float kRailRounding = 3.0F;
/** A zero wrap position wraps a description at the current content edge. */
constexpr float kAutomaticWrapPosition = 0.0F;

} // namespace

/** Draws a compact section title with an optional muted description. */
void header(const char* title, const char* description) noexcept {
    if (title == nullptr || ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const float railWidth = scaling::dpi::pixels(kRailWidth);
    const float railSpacing = scaling::dpi::pixels(kRailSpacing);
    ImGui::BeginGroup();
    ImGui::Dummy({railWidth, ImGui::GetTextLineHeight()});
    ImGui::SameLine(0.0F, railSpacing);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(title);
    if (description != nullptr && description[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(kAutomaticWrapPosition);
        ImGui::TextWrapped("%s", description);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::EndGroup();

    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float railVerticalInset = scaling::dpi::pixels(kRailVerticalInset);
    ImGui::GetWindowDrawList()->AddRectFilled(
        {minimum.x, minimum.y + railVerticalInset},
        {minimum.x + railWidth, maximum.y - railVerticalInset},
        ImGui::GetColorU32(ImGuiCol_CheckMark),
        scaling::dpi::pixels(kRailRounding));
}

} // namespace sunrise::core::ui::components::section
