#pragma once

namespace sunrise::core::ui::components::navigation {

/** Where the label sits, for list and compact navigation surfaces. */
enum class LabelAlignment {
    /** Leaves room for the leading selection rail used by vertical module lists. */
    leading,
    /** Centers compact peer choices, with no hover or selection shift. */
    centered,
};

/**
 * Draws a rounded navigation tab with hover and selection transitions.
 * @param label Visible label with an optional Dear ImGui ID suffix.
 * @param selected True when this tab owns the current content page.
 * @param width Final framebuffer width, or zero to use the width left.
 * @param height Final framebuffer height, or zero to use the scaled authored height.
 * @param alignment Where the label sits on the caller's navigation surface.
 * @return True when the mouse or keyboard fires the tab.
 */
[[nodiscard]] bool tab(const char* label,
                       bool selected,
                       float width = 0.0F,
                       float height = 0.0F,
                       LabelAlignment alignment = LabelAlignment::leading) noexcept;

} // namespace sunrise::core::ui::components::navigation
