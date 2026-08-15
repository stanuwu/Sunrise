/**
 * Subclass of the game's hidden raw-mouse sink window. The game registers raw mouse to its own
 * `Tiger Input Window`, not to the swap-chain output window, so every mouse move and button
 * arrives there as WM_INPUT and never reaches the render window's subclass. A visible interface
 * takes those messages; every other message is forwarded unchanged.
 */

#include <Windows.h>

#include <atomic>
#include <bit>

#include "../../../../core/ui/runtime/ui_visibility_runtime.h"
#include "input.h"

namespace sunrise::client::hooks::graphics::input {
namespace {

/** Class the game registers for its raw-mouse sink window. */
constexpr wchar_t kRawInputWindowClass[] = L"Tiger Input Window";

/** One active or retired subclass of that window and its exact forwarding target. */
struct RawBinding {
    HWND window{};
    WNDPROC original{};
    bool installed{};
};

RawBinding g_rawBinding{};
std::atomic_uint g_rawActiveCallbacks{};
SRWLOCK g_rawLock{SRWLOCK_INIT};

/**
 * Finds this process's own raw-input sink window. The class is registered per module, so another
 * process running the same game would otherwise match first.
 * @return The window, or null while the game has not created it yet.
 */
[[nodiscard]] HWND find_raw_input_window() noexcept {
    const DWORD self = GetCurrentProcessId();
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, kRawInputWindowClass, nullptr)) != nullptr) {
        DWORD owner = 0;
        (void)GetWindowThreadProcessId(window, &owner);
        if (owner == self) {
            return window;
        }
    }
    return nullptr;
}

/**
 * Takes raw input while the interface is visible and forwards everything else.
 * @return Default result for captured raw input, or the exact original procedure result.
 */
LRESULT CALLBACK raw_window_procedure(HWND window,
                                      UINT message,
                                      WPARAM word,
                                      LPARAM value) noexcept {
    g_rawActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
    AcquireSRWLockShared(&g_rawLock);
    const WNDPROC original = g_rawBinding.window == window ? g_rawBinding.original : nullptr;
    ReleaseSRWLockShared(&g_rawLock);

    // A captured message still goes to the default procedure, or the system keeps the raw-input
    // buffer alive. That is also the fallback when there is no procedure to forward to.
    const bool captured = message == WM_INPUT && core::ui::runtime::snapshot().visible;
    const bool forward = !captured && original != nullptr;
    const LRESULT result = forward ? CallWindowProcW(original, window, message, word, value)
                                   : DefWindowProcW(window, message, word, value);
    g_rawActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

} // namespace

/** Subclasses the raw-input sink window. */
bool install_raw_input_window() noexcept {
    AcquireSRWLockExclusive(&g_rawLock);
    if (g_rawBinding.installed) {
        ReleaseSRWLockExclusive(&g_rawLock);
        return true;
    }
    if (g_rawActiveCallbacks.load(std::memory_order_acquire) != 0) {
        // A retired procedure keeps its forwarding record until every old call returns.
        ReleaseSRWLockExclusive(&g_rawLock);
        return false;
    }
    ReleaseSRWLockExclusive(&g_rawLock);

    HWND const window = find_raw_input_window();
    if (window == nullptr) {
        return false;
    }

    AcquireSRWLockExclusive(&g_rawLock);
    if (g_rawBinding.installed) {
        ReleaseSRWLockExclusive(&g_rawLock);
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR original =
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&raw_window_procedure));
    if (original == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_rawLock);
        return false;
    }
    g_rawBinding = RawBinding{window, std::bit_cast<WNDPROC>(original), true};
    ReleaseSRWLockExclusive(&g_rawLock);
    return true;
}

/** Restores the original procedure only when Sunrise still owns the chain head. */
bool uninstall_raw_input_window() noexcept {
    AcquireSRWLockExclusive(&g_rawLock);
    if (!g_rawBinding.installed) {
        const bool idle = g_rawActiveCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_rawLock);
        return idle;
    }
    if (IsWindow(g_rawBinding.window) == FALSE) {
        g_rawBinding.installed = false;
        const bool idle = g_rawActiveCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_rawLock);
        return idle;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current = GetWindowLongPtrW(g_rawBinding.window, GWLP_WNDPROC);
    if (current == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_rawLock);
        return false;
    }
    if (current == reinterpret_cast<LONG_PTR>(g_rawBinding.original)) {
        // The window owner already restored our exact forwarding target during its lifecycle.
        g_rawBinding.installed = false;
        const bool idle = g_rawActiveCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_rawLock);
        return idle;
    }
    if (current != reinterpret_cast<LONG_PTR>(&raw_window_procedure)) {
        // A later subclass must not be overwritten because it owns the current chain head.
        ReleaseSRWLockExclusive(&g_rawLock);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR replaced = SetWindowLongPtrW(
        g_rawBinding.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_rawBinding.original));
    if (replaced == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_rawLock);
        return false;
    }
    // Retain the forwarding target until a later install replaces this retired record.
    g_rawBinding.installed = false;
    const bool idle = g_rawActiveCallbacks.load(std::memory_order_acquire) == 0;
    ReleaseSRWLockExclusive(&g_rawLock);
    return idle;
}

} // namespace sunrise::client::hooks::graphics::input
