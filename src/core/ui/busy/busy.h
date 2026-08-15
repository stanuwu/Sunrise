#pragma once

namespace sunrise::core::ui::busy {

/** Work that stalls the game long enough to look like a hang without a visible sign. */
enum class Task : unsigned {
    manifestBuild,
    initialization,
    contentExtraction,
    cacheWrite,
    count,
};

/**
 * Raises a task without waiting for a frame. Use when the caller does not stall the game.
 * Repeat calls are free, so a task that spans many calls is raised on each of them and ended
 * once the work it names is done.
 * @param task Work that is running. Safe from any thread.
 */
void raise(Task task) noexcept;

/**
 * Shows the overlay for one task, then waits until the game has presented it once.
 * A presented frame stays on screen while nothing runs, so the overlay survives a stall that
 * stops the frame loop. The wait is bounded, and is skipped when no frames are arriving.
 * @param task Work that has started. Safe from any thread.
 */
void begin(Task task) noexcept;

/**
 * Raises a task and reports whether the caller must let a frame pass before starting.
 * The presenting thread cannot wait for its own frame, so work on that thread raises the
 * overlay on one call and starts on a later one. Every deferral is bounded.
 * @param task Work that is about to start.
 * @return True while the overlay has not reached the screen yet.
 */
[[nodiscard]] bool raise_early(Task task) noexcept;

/** @param task Work that has finished. Safe from any thread. */
void end(Task task) noexcept;

/**
 * Draws the running-work overlay inside the caller's active Dear ImGui frame.
 * It is independent of interface visibility, so the user sees it without opening anything.
 * @return True when draw data was built for the overlay.
 */
[[nodiscard]] bool draw() noexcept;

/** Records one finished present. Call after the original Present returns. */
void confirm_presented() noexcept;

} // namespace sunrise::core::ui::busy
