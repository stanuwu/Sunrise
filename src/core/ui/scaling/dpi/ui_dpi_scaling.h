#pragma once

#include <Windows.h>

#include <imgui.h>

namespace sunrise::core::ui::scaling::dpi {

/** Display-scale result, held by value. */
struct Value {
    /** DPI after clamping to the supported range. */
    UINT effectiveDpi{USER_DEFAULT_SCREEN_DPI};
    /** Client-area width used to scale by resolution. */
    UINT viewportWidth{};
    /** Client-area height used to scale by resolution. */
    UINT viewportHeight{};
    /** Multiplier over the authored 96-DPI, 1920 by 1080 geometry. */
    float scale{1.0F};
};

/**
 * Works out the scale from DPI and client size. Changes no runtime state.
 * @param reportedDpi DPI from Windows. Must not be zero.
 * @param viewportWidth Client-area width in framebuffer pixels. Must not be zero.
 * @param viewportHeight Client-area height in framebuffer pixels. Must not be zero.
 * @param output Receives the clamped value.
 * @return True for valid inputs. output is unchanged on failure.
 */
[[nodiscard]] bool
calculate(UINT reportedDpi, UINT viewportWidth, UINT viewportHeight, Value& output) noexcept;

/**
 * Updates the scale from one window. Dear ImGui style values are left alone. The owning render
 * lock must serialize updates and reads.
 * @param window Current game output window.
 * @return True when a valid DPI or client size changes the published scale.
 */
[[nodiscard]] bool update(HWND window) noexcept;

/** @return Current multiplier over the authored geometry. */
[[nodiscard]] float current() noexcept;

/** @return The authored pixel value, scaled for the display. */
[[nodiscard]] float pixels(float authoredPixels) noexcept;

/** @return The authored pixel vector, scaled for the display. */
[[nodiscard]] ImVec2 pixels(const ImVec2& authoredPixels) noexcept;

/** Returns to the unscaled base after all UI render calls stop. */
void reset() noexcept;

} // namespace sunrise::core::ui::scaling::dpi
