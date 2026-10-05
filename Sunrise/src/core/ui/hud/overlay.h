#pragma once

#include <cstdint>

namespace sunrise::core::ui::hud {

/** Every HUD overlay, in the order the menu lists them and the corner stacks them. */
enum class Overlay : std::uint8_t {
    /** The Sunrise name, version and animated logo. */
    logoCard,
    /** How to open the menu, shown once beside the card shortly after start. */
    startupHint,
    /** Where the player is: activity, bubble, slice set and closest spawn. */
    currentStatus,
    /** The instances of the session the player is in. */
    session,
    /** Recent owned client messages at the Activity Host boundary. */
    sensorEvents,
    /** What the mission script VM is doing, per attached activity. */
    missionScript,
    count,
};

/** Every line of the current-status overlay, each with its own switch. */
enum class StatusLine : std::uint8_t {
    activity,
    bubble,
    sliceSet,
    closestSpawn,
    count,
};

/**
 * Resolves the switch file and applies the saved state. It runs at boot, before the first
 * frame, which is the only point another thread touches the switches.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the switch file path. The switches keep their values. */
void shutdown() noexcept;

/** @param overlay Overlay to name. @return Its menu label. */
[[nodiscard]] const char* display_name(Overlay overlay) noexcept;

/** @param overlay Overlay to read. @return True while it draws. */
[[nodiscard]] bool enabled(Overlay overlay) noexcept;

/** @param overlay Overlay to switch. @param on New switch state. */
void set_enabled(Overlay overlay, bool on) noexcept;

/** @param line Status line to name. @return Its menu label. */
[[nodiscard]] const char* display_name(StatusLine line) noexcept;

/** @param line Status line to read. @return True while it draws. */
[[nodiscard]] bool enabled(StatusLine line) noexcept;

/** @param line Status line to switch. @param on New switch state. */
void set_enabled(StatusLine line, bool on) noexcept;

/**
 * Draws every enabled overlay, stacked down the top-left corner. It runs whether the menu is
 * open or not.
 * @param interfaceEnabled Core UI enabled state. A disabled interface draws no overlay, because
 * the menu that switches them off cannot be opened either.
 * @return True when draw data was built for at least one overlay.
 */
[[nodiscard]] bool draw(bool interfaceEnabled) noexcept;

} // namespace sunrise::core::ui::hud
