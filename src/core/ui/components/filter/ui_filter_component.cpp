#include "ui_filter_component.h"

#include <algorithm>
#include <cstddef>
#include <imgui.h>

#include "../../animation/transition/ui_transition_animation.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "../drawing/drawing.h"

namespace sunrise::core::ui::components::filter {
namespace {

/** The hidden input label is scoped by the caller's stable filter ID. */
constexpr char kInputWidgetId[] = "##filter_value";
/** The hidden action label is scoped by the caller's stable filter ID. */
constexpr char kClearWidgetId[] = "##filter_clear";
/** A short action label leaves most horizontal space for filter text. */
constexpr char kClearLabel[] = "Clear";
/** An empty hint keeps the control valid when the caller passes no hint. */
constexpr char kEmptyHint[] = "";
/** 120 authored pixels keep inline filter text useful. */
constexpr float kMinimumInlineInputWidth = 120.0F;
/** One authored pixel keeps stacked controls valid inside a clipped parent. */
constexpr float kMinimumControlWidth = 1.0F;
/** 9 authored pixels on each side give the clear action a steady target. */
constexpr float kClearHorizontalPadding = 9.0F;
/** One authored pixel keeps outlines sharp over the standard field. */
constexpr float kBorderThickness = 1.0F;
/** Pointer hover adds half as much edge color as keyboard focus. */
constexpr float kHoverBorderWeight = 0.5F;
/** Clear hover shifts most of the idle button tone, but not to the selection color. */
constexpr float kClearHoverWeight = 0.72F;
/** Text fields respond quickly when the pointer enters or leaves. */
constexpr animation::transition::Rates kHoverRates{18.0F, 14.0F};
/** Keyboard focus settles more slowly, so it stays visible while typing. */
constexpr animation::transition::Rates kFocusRates{14.0F, 11.0F};
/** The compact clear action uses the fastest response in the filter row. */
constexpr animation::transition::Rates kClearHoverRates{22.0F, 17.0F};

/**
 * Draws the fixed-label clear action inside the caller's Dear ImGui ID scope.
 * @param width Button width kept back by the filter row.
 * @param height Button height matched to the text field.
 * @return True when the mouse or keyboard fires the action.
 */
[[nodiscard]] bool clear_button(float width, float height) noexcept {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImGuiID id = ImGui::GetID(kClearWidgetId);
    const bool pressed = ImGui::InvisibleButton(kClearWidgetId, {width, height});
    const float hover = animation::transition::update(
        id, animation::transition::Lane::hover, ImGui::IsItemHovered(), kClearHoverRates, 0.0F);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 fill = drawing::blend(style.Colors[ImGuiCol_Button],
                                       style.Colors[ImGuiCol_ButtonHovered],
                                       hover * kClearHoverWeight);
    const ImVec4 border =
        drawing::blend(style.Colors[ImGuiCol_Border], style.Colors[ImGuiCol_CheckMark], hover);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 maximum{position.x + width, position.y + height};
    drawList->AddRectFilled(position, maximum, ImGui::GetColorU32(fill), style.FrameRounding);
    drawList->AddRect(position,
                      maximum,
                      ImGui::GetColorU32(border),
                      style.FrameRounding,
                      ImDrawFlags_None,
                      scaling::dpi::pixels(kBorderThickness));

    const ImVec2 textSize = ImGui::CalcTextSize(kClearLabel);
    drawList->AddText(
        {position.x + ((width - textSize.x) * 0.5F), position.y + ((height - textSize.y) * 0.5F)},
        ImGui::GetColorU32(style.Colors[ImGuiCol_Text]),
        kClearLabel);
    return pressed;
}

} // namespace

/** Draws an animated text filter and a clear action without owning text storage. */
bool input(const char* id,
           const char* hint,
           char* buffer,
           std::size_t capacity,
           ImGuiInputTextFlags flags) noexcept {
    if (id == nullptr || buffer == nullptr || capacity == 0
        || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fieldHeight = ImGui::GetFrameHeight();
    const float availableWidth =
        (std::max)(ImGui::GetContentRegionAvail().x, scaling::dpi::pixels(kMinimumControlWidth));
    const float authoredClearWidth =
        ImGui::CalcTextSize(kClearLabel).x + (scaling::dpi::pixels(kClearHorizontalPadding) * 2.0F);
    const float clearWidth = (std::min)(authoredClearWidth, availableWidth);
    const float reservedWidth = clearWidth + style.ItemSpacing.x;
    const bool inlineControls =
        availableWidth >= (scaling::dpi::pixels(kMinimumInlineInputWidth) + reservedWidth);
    const float inputWidth = inlineControls ? availableWidth - reservedWidth : availableWidth;
    const ImGuiID inputId = ImGui::GetID(kInputWidgetId);

    ImGui::SetNextItemWidth(inputWidth);
    bool changed = ImGui::InputTextWithHint(
        kInputWidgetId, hint != nullptr ? hint : kEmptyHint, buffer, capacity, flags);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemActive();
    const ImVec2 fieldMinimum = ImGui::GetItemRectMin();
    const ImVec2 fieldMaximum = ImGui::GetItemRectMax();
    const float hover = animation::transition::update(
        inputId, animation::transition::Lane::hover, hovered, kHoverRates, 0.0F);
    const float focus = animation::transition::update(
        inputId, animation::transition::Lane::focus, focused, kFocusRates, 0.0F);
    const float borderWeight = (std::max)(hover * kHoverBorderWeight, focus);
    const ImVec4 border = drawing::blend(
        style.Colors[ImGuiCol_Border], style.Colors[ImGuiCol_CheckMark], borderWeight);
    ImGui::GetWindowDrawList()->AddRect(fieldMinimum,
                                        fieldMaximum,
                                        ImGui::GetColorU32(border),
                                        style.FrameRounding,
                                        ImDrawFlags_None,
                                        scaling::dpi::pixels(kBorderThickness));

    // Narrow parents stack the clear action so the input never shrinks below useful text width.
    if (inlineControls) {
        ImGui::SameLine(0.0F, style.ItemSpacing.x);
    }
    if (clear_button(clearWidth, fieldHeight) && buffer[0] != '\0') {
        buffer[0] = '\0';
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

} // namespace sunrise::core::ui::components::filter
