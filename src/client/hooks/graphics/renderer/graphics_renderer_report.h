#pragma once

namespace sunrise::client::hooks::graphics::renderer::report {

/** Stage of the presentation stack that produced an outcome. */
enum class Stage : unsigned {
    none,
    acquire,
    target,
    init,
    frame,
    shutdown,
};

/** Why a stage ended the way it did. Ordered by the step that produces it. */
enum class Reason : unsigned {
    none,
    // acquire
    window,
    windowThread,
    windowClass,
    description,
    device,
    context,
    // target
    backBuffer,
    view,
    viewFormat,
    // init
    layout,
    win32Backend,
    dx11Backend,
    windowInput,
    // frame
    fontScale,
    // shutdown
    deviceLost,
    rebuildTarget,
    surfaceLost,
};

/**
 * Writes one outcome the first time its reason comes up. Present calls this every frame.
 * Reasons are unique across stages, so one bit per reason is enough.
 */
void note(Stage stage, Reason reason) noexcept;

/** Records that the stack is active, so a later failure is written again. */
void note_active() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::report
