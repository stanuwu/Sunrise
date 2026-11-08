#pragma once

namespace sunrise::core::ui::hud::overlays::startup_hint {

/** @return The hint's fade for this frame, from 0 (nothing to draw) to 1 (fully shown). */
[[nodiscard]] float progress() noexcept;

/** Draws the startup hint inside the overlay window the stack has already started. */
void draw() noexcept;

} // namespace sunrise::core::ui::hud::overlays::startup_hint
