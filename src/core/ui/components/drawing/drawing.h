#pragma once

#include <algorithm>
#include <imgui.h>

namespace sunrise::core::ui::components::drawing {

/** @return The blend weight clamped to the normalized color range. */
[[nodiscard]] inline float normalized(float value) noexcept {
    return (std::clamp)(value, 0.0F, 1.0F);
}

/**
 * Blends two colors with no temporary buffers.
 * @param from Color returned at weight 0.
 * @param to Color returned at weight 1.
 * @param weight Normalized blend weight.
 * @return The blended color.
 */
[[nodiscard]] inline ImVec4 blend(const ImVec4& from, const ImVec4& to, float weight) noexcept {
    const float amount = normalized(weight);
    return {from.x + ((to.x - from.x) * amount),
            from.y + ((to.y - from.y) * amount),
            from.z + ((to.z - from.z) * amount),
            from.w + ((to.w - from.w) * amount)};
}

/**
 * Finds the visible part of an ImGui label before its hidden ID suffix.
 * @param label Null-terminated widget label.
 * @return End pointer for draw-list text submission.
 */
[[nodiscard]] inline const char* visible_text_end(const char* label) noexcept {
    if (label == nullptr) {
        return nullptr;
    }
    const char* cursor = label;
    while (*cursor != '\0' && !(cursor[0] == '#' && cursor[1] == '#')) {
        ++cursor;
    }
    return cursor;
}

} // namespace sunrise::core::ui::components::drawing
