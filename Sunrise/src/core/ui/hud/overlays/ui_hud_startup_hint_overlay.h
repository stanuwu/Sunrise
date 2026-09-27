#pragma once

namespace sunrise::core::ui::hud::overlays::startup_hint {

/**
 * Advances the hint's one-time lifetime by this frame: a short wait after the first HUD frame, a
 * bounded stay, and an early end once the main surface opens. It never comes back in the same run.
 * @return Current fade, from 0 (nothing to draw) to 1 (fully shown).
 */
[[nodiscard]] float progress() noexcept;

/** Draws the startup hint inside the overlay window the stack has already started. */
void draw() noexcept;

} // namespace sunrise::core::ui::hud::overlays::startup_hint
