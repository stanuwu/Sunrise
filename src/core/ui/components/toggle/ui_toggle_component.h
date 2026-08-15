#pragma once

namespace sunrise::core::ui::components::toggle {

/**
 * Draws a full-row toggle with animated track and thumb positions.
 * @param label Visible label with an optional Dear ImGui ID suffix.
 * @param value Setting value, read and written.
 * @param width Final framebuffer width, or zero to use the width left.
 * @return True when this frame changed the value.
 */
[[nodiscard]] bool control(const char* label, bool& value, float width = 0.0F) noexcept;

} // namespace sunrise::core::ui::components::toggle
