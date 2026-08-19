#pragma once

#include <Windows.h>

namespace sunrise::izanami::editor::ui {

/** Draws the Izanami Forge status page inside the active Core UI frame. */
void draw() noexcept;

/** Draws the standalone in-game Forge overlay outside the Sunrise module browser. */
[[nodiscard]] bool draw_standalone() noexcept;

/** @return True while the standalone Forge overlay should capture input. */
[[nodiscard]] bool standalone_visible() noexcept;

/** Sets standalone overlay visibility directly. */
[[nodiscard]] bool set_standalone_visible(bool visible) noexcept;

/** Handles the standalone Forge overlay hotkey. */
[[nodiscard]] bool toggle_standalone_for_key(UINT virtualKey) noexcept;

/** @return Windows virtual key used for direct Forge access. */
[[nodiscard]] UINT standalone_toggle_key() noexcept;

} // namespace sunrise::izanami::editor::ui
