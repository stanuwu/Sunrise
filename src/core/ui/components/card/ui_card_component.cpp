#include "ui_card_component.h"

#include <algorithm>
#include <imgui.h>

#include "../../animation/transition/ui_transition_animation.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "../drawing/drawing.h"

namespace sunrise::core::ui::components::card {
namespace {

/** One authored pixel keeps a child valid when its parent is almost fully clipped. */
constexpr float kMinimumExtent = 1.0F;
/** 9 authored pixels tell content cards apart from smaller controls. */
constexpr float kCornerRadius = 9.0F;
/** 12 by 10 authored pixels give card content a clear edge. */
constexpr ImVec2 kContentPadding{12.0F, 10.0F};
/** Hover shifts only a quarter of the panel tone, so large surfaces do not flash. */
constexpr float kHoverFillWeight = 0.25F;
/** Hover adds a light accent to the card edge. */
constexpr float kHoverBorderWeight = 0.32F;
/** One authored pixel separates adjacent cards without a heavy frame. */
constexpr float kBorderThickness = 1.0F;
/** 3 scoped style variables are restored right after the child is made. */
constexpr int kScopedStyleVariableCount = 3;
/** Cards ease in and out slowly because their surface can cover a large area. */
constexpr animation::transition::Rates kHoverRates{9.0F, 7.0F};
/** Card children must not write Dear ImGui settings beside Sunrise settings. */
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings;
/** Card content always keeps the authored inset instead of touching the drawn edge. */
constexpr ImGuiChildFlags kChildFlags = ImGuiChildFlags_AlwaysUseWindowPadding;

/**
 * Finds the card size from the request and the space left.
 * @param requested Caller size; a zero axis means automatic.
 * @return Positive child size Dear ImGui accepts.
 */
[[nodiscard]] ImVec2 resolve_size(const ImVec2& requested) noexcept {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float minimumExtent = scaling::dpi::pixels(kMinimumExtent);
    return {(std::max)(requested.x > 0.0F ? requested.x : available.x, minimumExtent),
            (std::max)(requested.y > 0.0F ? requested.y : available.y, minimumExtent)};
}

} // namespace

/**
 * Starts a padded card and records whether its Dear ImGui child needs an End.
 * @param id Stable non-null Dear ImGui child ID.
 * @param size Final framebuffer size; a zero axis takes the space left.
 */
Scope::Scope(const char* id, const ImVec2& size) noexcept {
    if (id == nullptr || ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const ImVec2 resolvedSize = resolve_size(size);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + resolvedSize.x, minimum.y + resolvedSize.y};
    const ImGuiID cardId = ImGui::GetID(id);
    const bool hovered = ImGui::IsMouseHoveringRect(minimum, maximum, true);
    const float hover = animation::transition::update(
        cardId, animation::transition::Lane::hover, hovered, kHoverRates, 0.0F);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 fill = drawing::blend(style.Colors[ImGuiCol_ChildBg],
                                       style.Colors[ImGuiCol_FrameBgHovered],
                                       hover * kHoverFillWeight);
    const ImVec4 border = drawing::blend(style.Colors[ImGuiCol_Border],
                                         style.Colors[ImGuiCol_CheckMark],
                                         hover * kHoverBorderWeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float cornerRadius = scaling::dpi::pixels(kCornerRadius);
    const float borderThickness = scaling::dpi::pixels(kBorderThickness);
    drawList->AddRectFilled(minimum, maximum, ImGui::GetColorU32(fill), cornerRadius);
    drawList->AddRect(minimum,
                      maximum,
                      ImGui::GetColorU32(border),
                      cornerRadius,
                      ImDrawFlags_None,
                      borderThickness);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, cornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, scaling::dpi::pixels(kContentPadding));
    visible_ = ImGui::BeginChild(id, resolvedSize, kChildFlags, kWindowFlags);
    started_ = true;
    ImGui::PopStyleVar(kScopedStyleVariableCount);
    ImGui::PopStyleColor();
}

/** Ends the child when construction started it in a current Dear ImGui context. */
Scope::~Scope() noexcept {
    if (started_) {
        ImGui::EndChild();
    }
}

/** @return True when Dear ImGui requests content submission for this card. */
bool Scope::visible() const noexcept {
    return visible_;
}

} // namespace sunrise::core::ui::components::card
