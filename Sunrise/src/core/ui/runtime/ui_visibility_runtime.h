#pragma once

#include <Windows.h>

#include "settings.h"

namespace sunrise::core::ui::runtime {

/** A copy of the UI visibility state, taken under the lock, for the drawing code. */
struct VisibilitySnapshot {
    bool initialized{};
    bool enabled{};
    bool visible{};
    /** Insert is the binding before initialize runs. */
    UINT toggleVirtualKey{VK_INSERT};
};

/**
 * Starts visibility from Core settings, after checking them.
 * @param settings Enabled state and Windows virtual-key binding.
 * @return True when the binding is a usable Windows virtual key.
 */
[[nodiscard]] bool initialize(const Settings& settings) noexcept;

/** Clears visibility and key binding state so the module can unload. */
void shutdown() noexcept;

/** @return A copy of the whole visibility state, taken under the lock. */
[[nodiscard]] VisibilitySnapshot snapshot() noexcept;

/**
 * Sets the menu visibility without synthesizing a key press.
 * @param visible New visibility
 * target.
 * @return True when visibility runtime is initialized and enabled.
 */
[[nodiscard]] bool set_visible(bool visible) noexcept;

/**
 * Applies one key press to the visibility state.
 * @param virtualKey Code from Client input
 * handling.
 * @return True only when the active binding matched and visibility changed.
 */
[[nodiscard]] bool toggle_for_key(UINT virtualKey) noexcept;

} // namespace sunrise::core::ui::runtime
