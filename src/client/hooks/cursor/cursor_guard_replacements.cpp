#include "cursor_guard_replacements.h"

#include "runtime.h"

namespace sunrise::client::hooks::cursor {
namespace {

SRWLOCK g_policyLock{SRWLOCK_INIT};
bool g_interfaceOpen{};
bool g_gameClipValid{};
RECT g_gameClip{};

} // namespace

std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::array<void*, kHandleCount> g_targets{};

/** Answers the game's cursor move while the interface is open. */
BOOL WINAPI set_cursor_pos(int x, int y) noexcept {
    AcquireSRWLockShared(&g_policyLock);
    const bool open = g_interfaceOpen;
    ReleaseSRWLockShared(&g_policyLock);
    if (open) {
        return kCallSucceeded;
    }

    const SetCursorPos next = original<SetCursorPos>(HookSlot::setCursorPos);
    if (next == nullptr) {
        return kCallSucceeded;
    }
    return next(x, y);
}

/** Frees the pointer while the interface is open and remembers the game's own bounds. */
BOOL WINAPI clip_cursor(const RECT* bounds) noexcept {
    AcquireSRWLockExclusive(&g_policyLock);
    g_gameClipValid = bounds != nullptr;
    if (g_gameClipValid) {
        g_gameClip = *bounds;
    }
    const bool open = g_interfaceOpen;
    ReleaseSRWLockExclusive(&g_policyLock);

    const ClipCursor next = original<ClipCursor>(HookSlot::clipCursor);
    if (next == nullptr) {
        return kCallSucceeded;
    }
    return next(open ? nullptr : bounds);
}

/** Applies one interface-visibility edge to the pointer. */
void apply_policy(bool visible) noexcept {
    AcquireSRWLockExclusive(&g_policyLock);
    if (g_interfaceOpen == visible) {
        ReleaseSRWLockExclusive(&g_policyLock);
        return;
    }
    g_interfaceOpen = visible;
    const bool restore = !visible && g_gameClipValid;
    const RECT bounds = g_gameClip;
    ReleaseSRWLockExclusive(&g_policyLock);

    // Win32 runs outside the policy lock so the hooked call cannot re-enter it.
    const ClipCursor next = original<ClipCursor>(HookSlot::clipCursor);
    if (next == nullptr) {
        return;
    }
    if (visible) {
        (void)next(nullptr);
        return;
    }
    if (restore) {
        (void)next(&bounds);
    }
}

/** Applies the cursor policy for the current interface visibility. */
void apply_visibility(bool visible) noexcept {
    apply_policy(visible);
}

} // namespace sunrise::client::hooks::cursor
