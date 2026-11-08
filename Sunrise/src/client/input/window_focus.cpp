/**
 * Whether the game holds focus. The test is the process, not one window.
 * The game has several windows, and which one is in front changes with the display mode.
 */

#include "window_focus.h"

#include <Windows.h>

#include "../diagnostics/test_input/test_input.h"

namespace sunrise::client::input {

/** Reports whether a window of this process is in front; a test process always counts. */
bool game_focused() noexcept {
    if (diagnostics::test_input::enabled()) {
        return true;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

} // namespace sunrise::client::input
