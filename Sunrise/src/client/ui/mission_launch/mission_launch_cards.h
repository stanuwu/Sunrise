#pragma once
#include <algorithm>
#include <cmath>
#include <imgui.h>

#include "mission_launch_art.h"

namespace sunrise::client::ui::mission_launch {
[[nodiscard]] inline float card_scale() noexcept {
    return ImGui::GetFontSize() / 16.0F;
}
[[nodiscard]] inline int grid_columns(float width) noexcept {
    return (std::clamp)(static_cast<int>(width / (184.0F * card_scale())), 1, 5);
}
/** Draw within a caller-owned slot, keeping native pixels aligned and inheriting the menu fade. */
[[nodiscard]] inline bool
draw_icon(state::build_data::activities::Icon icon, ImVec2 origin, float extent) noexcept {
    const auto texture = art::texture(icon);
    if (texture == ImTextureID_Invalid) {
        return false;
    }
    const auto framebuffer = ImGui::GetIO().DisplayFramebufferScale;
    const float density = (std::max)(1.0F, (std::max)(framebuffer.x, framebuffer.y));
    const auto image = art::display_size(icon, extent, density);
    const float x = std::round((origin.x + (extent - image.width) * 0.5F) * density) / density;
    const float y = std::round((origin.y + (extent - image.height) * 0.5F) * density) / density;
    ImGui::GetWindowDrawList()->AddImage(texture,
                                         {x, y},
                                         {x + image.width, y + image.height},
                                         {0, 0},
                                         {1, 1},
                                         ImGui::GetColorU32(ImVec4{1, 1, 1, 1}));
    return true;
}
/** One complete layout item owns the card bounds. Text/images only draw inside those bounds.
 * Never rewinds or extends a cursor, and does not allocate or resize a font. */
[[nodiscard]] inline bool
activity_card(const char* title,
              const char* subtitle,
              const char* footer,
              state::build_data::activities::Icon icon,
              bool selected,
              bool available = true,
              const char* artworkFallback = "Artwork unavailable") noexcept {
    const float scale = card_scale();
    const float width = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
    const bool clicked = ImGui::InvisibleButton(
        "##activity_card", {width, std::ceil(188.0F * scale)}, ImGuiButtonFlags_EnableNav);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    draw->AddRectFilled(min,
                        max,
                        ImGui::GetColorU32(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                        7.0F * scale);
    draw->AddRect(min,
                  max,
                  ImGui::GetColorU32(hot || selected ? ImGuiCol_CheckMark : ImGuiCol_Border),
                  7.0F * scale,
                  0,
                  hot || selected ? 2.0F : 1.0F);
    const float inset = 13.0F * scale;
    const ImVec2 textMin{min.x + inset, min.y + 91.0F * scale};
    const ImVec2 textMax{max.x - inset, max.y - inset};
    draw->PushClipRect({min.x + inset, min.y + inset}, textMax, true);
    if (!draw_icon(icon, {min.x + inset, min.y + 12.0F * scale}, 66.0F * scale)) {
        draw->AddText({min.x + inset, min.y + 35.0F * scale},
                      ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      artworkFallback);
    }
    draw->PushClipRect(textMin, {textMax.x, textMin.y + ImGui::GetTextLineHeight() * 2.0F}, true);
    draw->AddText(ImGui::GetFont(),
                  ImGui::GetFontSize(),
                  textMin,
                  ImGui::GetColorU32(available ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                  title,
                  nullptr,
                  (std::max)(1.0F, textMax.x - textMin.x));
    draw->PopClipRect();
    draw->AddText(
        {textMin.x, min.y + 137.0F * scale}, ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle);
    draw->AddText({textMin.x, min.y + 157.0F * scale},
                  ImGui::GetColorU32(available ? ImGuiCol_TextDisabled : ImGuiCol_PlotHistogram),
                  footer);
    draw->PopClipRect();
    return clicked;
}
} // namespace sunrise::client::ui::mission_launch
