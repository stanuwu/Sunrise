#include "ui_navigation_component.h"

#include <algorithm>
#include <imgui.h>

#include "../../animation/transition/ui_transition_animation.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "../drawing/drawing.h"

namespace sunrise::core::ui::components::navigation {
namespace {

/** 34 authored pixels give a tab one clear pointer target. */
constexpr float kDefaultHeight = 34.0F;
/** One authored pixel keeps a tab valid when its parent is clipped. */
constexpr float kMinimumWidth = 1.0F;
/** 16 authored pixels keep the rail positive between its end insets. */
constexpr float kMinimumHeight = 16.0F;
/** 7 authored pixels match the smaller rounded Sunrise controls. */
constexpr float kCornerRadius = 7.0F;
/** 10 authored pixels separate the tab label from its selection rail. */
constexpr float kLabelInset = 10.0F;
/** 4 authored pixels move selected text away from the active rail. */
constexpr float kSelectedTextShift = 4.0F;
/** 2 authored pixels give hovered text a smaller move than selected text. */
constexpr float kHoveredTextShift = 2.0F;
/** 3 authored pixels keep the orange rail visible without taking label space. */
constexpr float kSelectionRailWidth = 3.0F;
/** 7 authored pixels shorten each end of the rail inside the rounded tab. */
constexpr float kSelectionRailInset = 7.0F;
/** Hover fill stays weaker than the selected fill. */
constexpr float kHoverFillOpacity = 0.45F;
/** Selection fill stays see-through, so white labels keep their contrast. */
constexpr float kSelectionFillOpacity = 0.72F;
/** One authored pixel keeps outlines sharp at common display scale factors. */
constexpr float kBorderThickness = 1.0F;
/** Hover reaches its target quickly and leaves more gently. */
constexpr animation::transition::Rates kHoverRates{18.0F, 12.0F};
/** Selection moves a bit slower, so a page change reads as a state change. */
constexpr animation::transition::Rates kSelectionRates{13.0F, 10.0F};

} // namespace

/** Draws a rounded navigation tab with hover and selection transitions. */
bool tab(const char* label,
         bool selected,
         float width,
         float height,
         LabelAlignment alignment) noexcept {
    if (label == nullptr || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float minimumWidth = scaling::dpi::pixels(kMinimumWidth);
    const float minimumHeight = scaling::dpi::pixels(kMinimumHeight);
    const float defaultHeight = scaling::dpi::pixels(kDefaultHeight);
    const float resolvedWidth =
        (std::max)(width > 0.0F ? width : ImGui::GetContentRegionAvail().x, minimumWidth);
    const float resolvedHeight = (std::max)(height > 0.0F ? height : defaultHeight, minimumHeight);
    const ImVec2 size{resolvedWidth, resolvedHeight};
    const ImGuiID id = ImGui::GetID(label);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();

    const float hover = animation::transition::update(
        id, animation::transition::Lane::hover, hovered, kHoverRates, 0.0F);
    const float selection = animation::transition::update(id,
                                                          animation::transition::Lane::selection,
                                                          selected,
                                                          kSelectionRates,
                                                          selected ? 1.0F : 0.0F);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fillWeight =
        (std::max)(hover * kHoverFillOpacity, selection * kSelectionFillOpacity);
    ImVec4 fill = drawing::blend(
        style.Colors[ImGuiCol_ChildBg], style.Colors[ImGuiCol_HeaderHovered], fillWeight);
    fill = drawing::blend(fill, style.Colors[ImGuiCol_Header], selection);
    const ImVec4 border =
        drawing::blend(style.Colors[ImGuiCol_Border], style.Colors[ImGuiCol_CheckMark], selection);
    const ImVec4 text = drawing::blend(style.Colors[ImGuiCol_TextDisabled],
                                       style.Colors[ImGuiCol_Text],
                                       (std::max)(hover, selection));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 maximum{position.x + size.x, position.y + size.y};
    const float cornerRadius = scaling::dpi::pixels(kCornerRadius);
    const float borderThickness = scaling::dpi::pixels(kBorderThickness);
    drawList->AddRectFilled(position, maximum, ImGui::GetColorU32(fill), cornerRadius);
    drawList->AddRect(position,
                      maximum,
                      ImGui::GetColorU32(border),
                      cornerRadius,
                      ImDrawFlags_None,
                      borderThickness);

    const float railStrength = (std::max)(hover * kHoverFillOpacity, selection);
    const float railWidth = scaling::dpi::pixels(kSelectionRailWidth);
    const float railInset = scaling::dpi::pixels(kSelectionRailInset);
    ImVec4 rail = style.Colors[ImGuiCol_CheckMark];
    rail.w *= railStrength;
    drawList->AddRectFilled({position.x, position.y + railInset},
                            {position.x + railWidth, maximum.y - railInset},
                            ImGui::GetColorU32(rail),
                            railWidth);

    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const float textShift = alignment == LabelAlignment::leading
                                ? (selection * scaling::dpi::pixels(kSelectedTextShift))
                                      + (hover * scaling::dpi::pixels(kHoveredTextShift))
                                : 0.0F;
    const float textX = alignment == LabelAlignment::centered
                            ? position.x + ((size.x - textSize.x) * 0.5F)
                            : position.x + scaling::dpi::pixels(kLabelInset) + textShift;
    const ImVec2 textPosition{textX, position.y + ((size.y - textSize.y) * 0.5F)};
    drawList->AddText(
        textPosition, ImGui::GetColorU32(text), label, drawing::visible_text_end(label));
    return pressed;
}

} // namespace sunrise::core::ui::components::navigation
