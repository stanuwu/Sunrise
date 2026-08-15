#include "ui_dpi_scaling.h"

#include <Windows.h>

#include <algorithm>
#include <imgui.h>

namespace sunrise::core::ui::scaling::dpi {
namespace {

/** Windows treats 96 DPI as unscaled. */
constexpr UINT kAuthoredDpi = USER_DEFAULT_SCREEN_DPI;
/** Sunrise geometry is authored for a 1920-pixel-wide game viewport. */
constexpr UINT kAuthoredViewportWidth = 1920;
/** Sunrise geometry is authored for a 1080-pixel-tall game viewport. */
constexpr UINT kAuthoredViewportHeight = 1080;
/** 72 DPI keeps geometry at 75 percent of authored size or more. */
constexpr UINT kMinimumDpi = 72;
/** 240 DPI caps geometry at 250 percent on very dense displays. */
constexpr UINT kMaximumDpi = 240;
/** Windows returns zero when a window has no valid DPI value. */
constexpr UINT kInvalidDpi = 0;
/** A zero client-area side gives no usable scale. */
constexpr UINT kInvalidViewportExtent = 0;
/** 18 over 16 gives comfortable text at 1080p. */
constexpr float kAuthoredComfortScale = 1.125F;
/** 80 percent keeps controls usable on small game viewports. */
constexpr float kMinimumViewportScale = 0.8F;
/** 175 percent caps growth from framebuffer size alone. */
constexpr float kMaximumViewportScale = 1.75F;
/** 90 percent keeps a 720p UI readable and still fitting. */
constexpr float kMinimumScale = kMinimumViewportScale * kAuthoredComfortScale;
/** 250 percent caps UI geometry on very dense or large framebuffers. */
constexpr float kMaximumScale = 2.5F;

/** One value written at once, so the scale is never partly applied. */
Value g_current{};

} // namespace

/** Works out the scale from DPI and client size. Changes no runtime state. */
bool calculate(UINT reportedDpi, UINT viewportWidth, UINT viewportHeight, Value& output) noexcept {
    if (reportedDpi == kInvalidDpi || viewportWidth == kInvalidViewportExtent
        || viewportHeight == kInvalidViewportExtent) {
        return false;
    }
    const UINT effectiveDpi = (std::clamp)(reportedDpi, kMinimumDpi, kMaximumDpi);
    const float dpiScale = static_cast<float>(effectiveDpi) / static_cast<float>(kAuthoredDpi);
    const float widthScale =
        static_cast<float>(viewportWidth) / static_cast<float>(kAuthoredViewportWidth);
    const float heightScale =
        static_cast<float>(viewportHeight) / static_cast<float>(kAuthoredViewportHeight);
    // The smaller axis stops odd aspect ratios from scaling past the space there is.
    const float viewportScale = (std::clamp)((std::min)(widthScale, heightScale),
                                             kMinimumViewportScale,
                                             kMaximumViewportScale);
    // At base DPI the UI may shrink. Above it, DPI and framebuffer scale do not stack.
    const float environmentalScale =
        dpiScale <= 1.0F ? viewportScale : (std::max)(viewportScale, dpiScale);
    const float scale =
        (std::clamp)(environmentalScale * kAuthoredComfortScale, kMinimumScale, kMaximumScale);
    output = {effectiveDpi, viewportWidth, viewportHeight, scale};
    return true;
}

/** Updates the scale from one window. Dear ImGui style values are left alone. */
bool update(HWND window) noexcept {
    if (window == nullptr) {
        return false;
    }
    const UINT reportedDpi = GetDpiForWindow(window);
    RECT clientArea{};
    if (GetClientRect(window, &clientArea) == FALSE || clientArea.right <= clientArea.left
        || clientArea.bottom <= clientArea.top) {
        return false;
    }
    const UINT viewportWidth = static_cast<UINT>(clientArea.right - clientArea.left);
    const UINT viewportHeight = static_cast<UINT>(clientArea.bottom - clientArea.top);
    Value calculated{};
    if (!calculate(reportedDpi, viewportWidth, viewportHeight, calculated)) {
        return false;
    }
    if (calculated.effectiveDpi == g_current.effectiveDpi
        && calculated.viewportWidth == g_current.viewportWidth
        && calculated.viewportHeight == g_current.viewportHeight) {
        return false;
    }
    const bool scaleChanged = calculated.scale != g_current.scale;
    g_current = calculated;
    return scaleChanged;
}

/** @return Current multiplier over the authored geometry. */
float current() noexcept {
    return g_current.scale;
}

/** @return The authored pixel value, scaled for the display. */
float pixels(float authoredPixels) noexcept {
    return authoredPixels * current();
}

/** @return The authored pixel vector, scaled for the display. */
ImVec2 pixels(const ImVec2& authoredPixels) noexcept {
    const float scale = current();
    return {authoredPixels.x * scale, authoredPixels.y * scale};
}

/** Returns to the unscaled base after all UI render calls stop. */
void reset() noexcept {
    g_current = {};
}

} // namespace sunrise::core::ui::scaling::dpi
