#include <Windows.h>

#include <atomic>
#include <bit>

#include "../../../../core/ui/layout/credits/sunrise_credits_badge.h"
#include "../../../../core/ui/modules/logs/logs.h"
#include "../../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../../izanami/editor/ui/izanami_panel.h"
#include "../renderer/renderer.h"
#include "input.h"

namespace sunrise::client::hooks::graphics::input {
namespace {

/** A nonzero result stops captured input from reaching the game. */
constexpr LRESULT kCapturedMessageResult = 1;

/** One window subclass, live or retired, and the procedure it forwards to. */
struct Binding {
    HWND window{};
    WNDPROC original{};
    bool installed{};
};

Binding g_binding{};
std::atomic_uint g_activeCallbacks{};
SRWLOCK g_inputLock{SRWLOCK_INIT};

/** Counts active procedure calls so teardown can be retried before module unload. */
class CallbackGuard final {
public:
    /** Counts this call in before any binding state is read. */
    CallbackGuard() noexcept {
        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    /** Counts this call out only after every deferred action has finished. */
    ~CallbackGuard() noexcept {
        g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

/**
 * Finds the original procedure from a live or retired subclass record.
 * @return Original procedure for the matching record, or null when nothing matches.
 */
[[nodiscard]] WNDPROC original_for(HWND window) noexcept {
    AcquireSRWLockShared(&g_inputLock);
    const WNDPROC original = g_binding.window == window ? g_binding.original : nullptr;
    ReleaseSRWLockShared(&g_inputLock);
    return original;
}

/** @param message Win32 message ID. @return True for the four ordinary key messages. */
[[nodiscard]] bool is_key_message(UINT message) noexcept {
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN
           || message == WM_SYSKEYUP;
}

/**
 * Calls the original procedure. The input lock must be released first, since game code runs.
 * @param original Procedure captured when the message entered this subclass.
 * @param message Win32 message ID.
 * @return Result from the original procedure, or DefWindowProc when there is none.
 */
[[nodiscard]] LRESULT
forward(WNDPROC original, HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    if (original == nullptr) {
        return DefWindowProcW(window, message, word, value);
    }
    return CallWindowProcW(original, window, message, word, value);
}

/**
 * Lets the presentation message through and captures only the input the UI asked for.
 * @param message Win32 message ID.
 * @return The capture result, or the original procedure's result.
 */
LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    const CallbackGuard callbackGuard;
    const WNDPROC original = original_for(window);
    if (message == kRequiredForwardMessage) {
        const LRESULT result = forward(original, window, message, word, value);
        // Presentation locks are released first, because a deferred action can re-enter here.
        renderer::dispatch_pending_input_release(window);
        core::ui::modules::logs::dispatch_pending_copy(window);
        core::ui::layout::credits::dispatch_pending();
        return result;
    }
    if (message == WM_CAPTURECHANGED) {
        // ReleaseCapture sends this synchronously while Dear ImGui owns the renderer lock.
        return forward(original, window, message, word, value);
    }
    // The toggle key is ours in both states, so the game cannot see half of a press.
    const bool coreToggleKey =
        is_key_message(message)
        && static_cast<UINT>(word) == core::ui::runtime::snapshot().toggleVirtualKey;
    const bool forgeToggleKey =
        is_key_message(message)
        && static_cast<UINT>(word) == ::sunrise::izanami::editor::ui::standalone_toggle_key();
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        (void)core::ui::runtime::toggle_for_key(static_cast<UINT>(word));
        (void)::sunrise::izanami::editor::ui::toggle_standalone_for_key(static_cast<UINT>(word));
    }
    if (renderer::handle_window_message(window, message, word, value) || coreToggleKey
        || forgeToggleKey) {
        // A visible UI takes every input message; the game's procedure never sees it.
        renderer::dispatch_pending_input_release(window);
        if (message == WM_INPUT) {
            // The default procedure must run or the system keeps the raw-input buffer alive.
            return DefWindowProcW(window, message, word, value);
        }
        return kCapturedMessageResult;
    }
    const LRESULT result = forward(original, window, message, word, value);
    renderer::dispatch_pending_input_release(window);
    return result;
}

} // namespace

/** Subclasses the output window on this thread. Does not assume a class name. */
bool install(HWND window) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE) {
        return false;
    }
    AcquireSRWLockExclusive(&g_inputLock);
    if (g_binding.installed) {
        const bool sameWindow = g_binding.window == window;
        ReleaseSRWLockExclusive(&g_inputLock);
        return sameWindow;
    }
    if (g_activeCallbacks.load(std::memory_order_acquire) != 0) {
        // A retired procedure keeps its forwarding record until every old call returns.
        ReleaseSRWLockExclusive(&g_inputLock);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR original =
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_procedure));
    if (original == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_inputLock);
        return false;
    }
    g_binding = Binding{window, std::bit_cast<WNDPROC>(original), true};
    ReleaseSRWLockExclusive(&g_inputLock);
    return true;
}

/** Restores the original procedure only when Sunrise still owns the chain head. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_inputLock);
    if (!g_binding.installed) {
        const bool idle = g_activeCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_inputLock);
        return idle;
    }
    if (IsWindow(g_binding.window) == FALSE) {
        g_binding.installed = false;
        const bool idle = g_activeCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_inputLock);
        return idle;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR current = GetWindowLongPtrW(g_binding.window, GWLP_WNDPROC);
    const LONG_PTR replacement = reinterpret_cast<LONG_PTR>(&window_procedure);
    if (current == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_inputLock);
        return false;
    }
    if (current == reinterpret_cast<LONG_PTR>(g_binding.original)) {
        // The window owner already put our forwarding target back itself.
        g_binding.installed = false;
        const bool idle = g_activeCallbacks.load(std::memory_order_acquire) == 0;
        ReleaseSRWLockExclusive(&g_inputLock);
        return idle;
    }
    if (current != replacement) {
        // A later subclass owns the chain head now, so do not overwrite it.
        ReleaseSRWLockExclusive(&g_inputLock);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR replaced = SetWindowLongPtrW(
        g_binding.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_binding.original));
    if (replaced == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseSRWLockExclusive(&g_inputLock);
        return false;
    }
    // Keep the forwarding target until a later install replaces this retired record.
    g_binding.installed = false;
    const bool idle = g_activeCallbacks.load(std::memory_order_acquire) == 0;
    ReleaseSRWLockExclusive(&g_inputLock);
    return idle;
}

/** Checks whether Sunrise is still installed, or still sits below a later subclass. */
bool active(HWND window) noexcept {
    AcquireSRWLockShared(&g_inputLock);
    bool installed = g_binding.installed && g_binding.window == window && IsWindow(window) != FALSE
                     && IsWindowVisible(window) != FALSE;
    if (installed) {
        const LONG_PTR current = GetWindowLongPtrW(window, GWLP_WNDPROC);
        if (current == 0) {
            installed = false;
        } else {
            // A different non-original head is a later subclass that may still forward here.
            installed = current != reinterpret_cast<LONG_PTR>(g_binding.original);
        }
    }
    ReleaseSRWLockShared(&g_inputLock);
    return installed;
}

} // namespace sunrise::client::hooks::graphics::input
