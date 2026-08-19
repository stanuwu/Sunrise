#include "ui_visibility_runtime.h"

#include <Windows.h>

namespace sunrise::core::ui::runtime {
namespace {

/** Usable Windows virtual-key codes start at 1. */
constexpr UINT kFirstVirtualKey = 0x01;
/** Windows reserves 0xFF, so 0xFE is the last usable virtual-key code. */
constexpr UINT kLastVirtualKey = 0xFE;
/** The menu starts closed, so startup never takes game input focus. */
constexpr bool kInitialVisibility = false;

VisibilitySnapshot g_state{};
SRWLOCK g_visibilityLock{SRWLOCK_INIT};

/** @return True when the code is a usable Windows virtual key. */
[[nodiscard]] bool is_valid_virtual_key(UINT virtualKey) noexcept {
    return virtualKey >= kFirstVirtualKey && virtualKey <= kLastVirtualKey;
}

} // namespace

/** Starts visibility from Core settings, after checking them. */
bool initialize(const Settings& settings) noexcept {
    if (!is_valid_virtual_key(settings.toggleVirtualKey)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_visibilityLock);
    g_state.initialized = true;
    g_state.enabled = settings.enabled;
    g_state.visible = kInitialVisibility;
    g_state.toggleVirtualKey = settings.toggleVirtualKey;
    ReleaseSRWLockExclusive(&g_visibilityLock);
    return true;
}

/** Clears visibility and key binding state so the module can unload. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_visibilityLock);
    g_state = {};
    ReleaseSRWLockExclusive(&g_visibilityLock);
}

/** @return A copy of the whole visibility state, taken under the lock. */
VisibilitySnapshot snapshot() noexcept {
    AcquireSRWLockShared(&g_visibilityLock);
    const VisibilitySnapshot result = g_state;
    ReleaseSRWLockShared(&g_visibilityLock);
    return result;
}

/** Sets the menu visibility directly. */
bool set_visible(bool visible) noexcept {
    AcquireSRWLockExclusive(&g_visibilityLock);
    const bool handled = g_state.initialized && g_state.enabled;
    if (handled) {
        g_state.visible = visible;
    }
    ReleaseSRWLockExclusive(&g_visibilityLock);
    return handled;
}

/** Applies one key press to the visibility state. */
bool toggle_for_key(UINT virtualKey) noexcept {
    AcquireSRWLockExclusive(&g_visibilityLock);
    const bool handled =
        g_state.initialized && g_state.enabled && virtualKey == g_state.toggleVirtualKey;
    if (handled) {
        g_state.visible = !g_state.visible;
    }
    ReleaseSRWLockExclusive(&g_visibilityLock);
    return handled;
}

} // namespace sunrise::core::ui::runtime
