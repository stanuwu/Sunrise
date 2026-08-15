#pragma once

namespace sunrise::core::ui::components::section {

/**
 * Draws a compact section title with an optional muted description.
 * @param title Visible section title.
 * @param description Optional text shown below the title.
 */
void header(const char* title, const char* description = nullptr) noexcept;

} // namespace sunrise::core::ui::components::section
