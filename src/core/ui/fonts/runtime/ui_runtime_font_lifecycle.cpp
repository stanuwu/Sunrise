#include "ui_runtime_font_lifecycle.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <imgui.h>

#include "../installed/ui_installed_font_reader.h"

namespace sunrise::core::ui::fonts::runtime {
namespace {

/** 8 pixels refuses unreadable or near-zero authored sizes. */
constexpr float kMinimumBasePixelSize = 8.0F;
/** 64 pixels caps atlas work from bad authored sizes. */
constexpr float kMaximumBasePixelSize = 64.0F;
/** 0.75 matches the UI geometry policy's smallest DPI scale. */
constexpr float kMinimumDpiScale = 0.75F;
/** 2.5 matches the UI geometry policy's largest DPI scale. */
constexpr float kMaximumDpiScale = 2.5F;
/** 1 is the authored 96-DPI scale and the reset value. */
constexpr float kAuthoredScale = 1.0F;
/** 2x rasterization keeps scaled text crisp with no rebuild on a DPI change. */
constexpr float kRasterizerDensity = 2.0F;

/** Ownership held while the atlas borrows installed-font bytes. */
struct State {
    bool initialized{};
    Source source{Source::unavailable};
    ImGuiContext* context{};
    ImFontAtlas* atlas{};
    float priorBasePixelSize{};
    float priorMainScale{kAuthoredScale};
    float basePixelSize{};
    float scale{kAuthoredScale};
};

State g_state{};
SRWLOCK g_fontLock{SRWLOCK_INIT};

/** @return True when the authored font height is inside the size policy. */
[[nodiscard]] bool valid_base_size(float value) noexcept {
    return std::isfinite(value) && value >= kMinimumBasePixelSize && value <= kMaximumBasePixelSize;
}

/** @return True when the UI multiplier can be safely clamped. */
[[nodiscard]] bool valid_scale(float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

/**
 * Adds the built-in vector fallback at the same authored size.
 * @param atlas Current unlocked Core-owned font atlas.
 * @param basePixelSize Authored 96-DPI font height.
 * @return The added fallback font, or null when the ImGui allocation fails.
 */
[[nodiscard]] ImFont* add_embedded_font(ImFontAtlas& atlas, float basePixelSize) noexcept {
    ImFontConfig config{};
    config.SizePixels = basePixelSize;
    config.RasterizerDensity = kRasterizerDensity;
    return atlas.AddFontDefaultVector(&config);
}

} // namespace

/** Adds one installed or embedded font to the current empty Dear ImGui context. */
bool initialize(HMODULE module, float basePixelSize) noexcept {
    AcquireSRWLockExclusive(&g_fontLock);
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (g_state.initialized) {
        const bool active = context != nullptr && context == g_state.context;
        ReleaseSRWLockExclusive(&g_fontLock);
        return active;
    }
    if (context == nullptr || !valid_base_size(basePixelSize)) {
        ReleaseSRWLockExclusive(&g_fontLock);
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    if (atlas == nullptr || atlas->Locked) {
        ReleaseSRWLockExclusive(&g_fontLock);
        return false;
    }

    const float priorBasePixelSize = ImGui::GetStyle().FontSizeBase;
    const float priorMainScale = ImGui::GetStyle().FontScaleMain;
    atlas->Clear();
    io.FontDefault = nullptr;

    installed::DataView installedData{};
    Source source = Source::embeddedDefault;
    ImFont* font = nullptr;
    if (installed::load(module, installedData)) {
        ImFontConfig config{};
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = kRasterizerDensity;
        font = atlas->AddFontFromMemoryTTF(
            installedData.bytes, installedData.byteCount, basePixelSize, &config);
        source = Source::installed;
    }
    if (font == nullptr) {
        // A failed installed source is dropped before the fallback is added.
        atlas->Clear();
        installed::clear();
        font = add_embedded_font(*atlas, basePixelSize);
        source = Source::embeddedDefault;
    }
    if (font == nullptr) {
        atlas->Clear();
        installed::clear();
        ImGui::GetStyle().FontSizeBase = priorBasePixelSize;
        ImGui::GetStyle().FontScaleMain = priorMainScale;
        ReleaseSRWLockExclusive(&g_fontLock);
        return false;
    }

    io.FontDefault = font;
    ImGui::GetStyle().FontSizeBase = basePixelSize;
    ImGui::GetStyle().FontScaleMain = kAuthoredScale;
    g_state = {true,
               source,
               context,
               atlas,
               priorBasePixelSize,
               priorMainScale,
               basePixelSize,
               kAuthoredScale};
    ReleaseSRWLockExclusive(&g_fontLock);
    return true;
}

/** Sets one absolute UI multiplier without compounding prior frame values. */
bool apply_scale(float scale) noexcept {
    AcquireSRWLockExclusive(&g_fontLock);
    if (!g_state.initialized || ImGui::GetCurrentContext() != g_state.context
        || !valid_scale(scale)) {
        ReleaseSRWLockExclusive(&g_fontLock);
        return false;
    }

    g_state.scale = (std::clamp)(scale, kMinimumDpiScale, kMaximumDpiScale);
    // Assigning the authored-relative value keeps scale from compounding across frames.
    ImGui::GetStyle().FontScaleMain = g_state.scale;
    ReleaseSRWLockExclusive(&g_fontLock);
    return true;
}

/** Clears the owned atlas, then wipes the borrowed installed-font bytes. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_fontLock);
    if (!g_state.initialized) {
        installed::clear();
        ReleaseSRWLockExclusive(&g_fontLock);
        return true;
    }
    if (ImGui::GetCurrentContext() != g_state.context || g_state.atlas == nullptr
        || g_state.atlas->Locked || ImGui::GetIO().Fonts != g_state.atlas) {
        ReleaseSRWLockExclusive(&g_fontLock);
        return false;
    }

    ImGui::GetIO().FontDefault = nullptr;
    g_state.atlas->Clear();
    ImGui::GetStyle().FontSizeBase = g_state.priorBasePixelSize;
    ImGui::GetStyle().FontScaleMain = g_state.priorMainScale;
    installed::clear();
    g_state = {};
    ReleaseSRWLockExclusive(&g_fontLock);
    return true;
}

/** @return One snapshot of the font source, size, and scale, read under the lock. */
Snapshot snapshot() noexcept {
    AcquireSRWLockShared(&g_fontLock);
    const Snapshot result{g_state.initialized,
                          g_state.source,
                          installed::byte_count(),
                          g_state.basePixelSize,
                          g_state.scale};
    ReleaseSRWLockShared(&g_fontLock);
    return result;
}

} // namespace sunrise::core::ui::fonts::runtime
