/**
 * Whether the game holds focus. The test is the process, not one window.
 * The game has several windows, and which one is in front changes with the display mode.
 */

#include "window_focus.h"

#include <Windows.h>

namespace sunrise::client::input {

/** Reports whether a window of this process is in front. */
bool game_focused() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

} // namespace sunrise::client::input
