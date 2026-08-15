#pragma once

#include <Windows.h>

#include <cstddef>

namespace sunrise::core::ui::fonts::runtime {

/** 16 pixels is the authored text height at the unscaled 96-DPI layout. */
inline constexpr float kAuthoredBasePixelSize = 16.0F;

/** Font source chosen for the current Dear ImGui atlas. */
enum class Source {
    /** No font lifecycle is active. */
    unavailable,
    /** Dear ImGui's built-in scalable font is active. */
    embeddedDefault,
    /** The installed game font is borrowed from fixed Sunrise storage. */
    installed,
};

/** Font lifecycle state read under the lock. Carries no font bytes or file paths. */
struct Snapshot {
    bool initialized{};
    Source source{Source::unavailable};
    std::size_t installedByteCount{};
    float basePixelSize{};
    float scale{};
};

/**
 * Adds one installed or embedded font to the current empty Dear ImGui context.
 * Call before renderer backend initialization and the first frame.
 * @param module Loaded fallback module, or null to use only the process image.
 * @param basePixelSize Authored font height at 96 DPI.
 * @return True when either the installed face or embedded fallback is active.
 */
[[nodiscard]] bool initialize(HMODULE module,
                              float basePixelSize = kAuthoredBasePixelSize) noexcept;

/**
 * Sets one absolute UI multiplier without compounding prior frame values.
 * Call before Dear ImGui starts the next frame.
 * @param scale Requested text multiplier relative to the authored size.
 * @return True when the owned context accepted the clamped scale.
 */
[[nodiscard]] bool apply_scale(float scale) noexcept;

/**
 * Clears the owned atlas, then wipes the borrowed installed-font bytes.
 * Call after renderer backend shutdown and before destroying the ImGui context.
 * @return False when the owning context is not current or the atlas is locked.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return One snapshot of the font source, size, and scale, read under the lock. */
[[nodiscard]] Snapshot snapshot() noexcept;

} // namespace sunrise::core::ui::fonts::runtime
