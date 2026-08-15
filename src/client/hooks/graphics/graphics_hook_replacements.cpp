#include "graphics_hook_replacements.h"

#include <atomic>

#include "../../../core/ui/busy/busy.h"
#include "graphics_hook_lifecycle.h"

namespace sunrise::client::hooks::graphics::replacement {
namespace {

/**
 * Publishes one replacement call before it can wait behind hook teardown.
 * @return True when this call may enter the Sunrise renderer.
 */
__declspec(noinline) bool enter_hook_call() noexcept {
    if (g_hookCallDepth == 0) {
        // The count is published before the lock wait, so a queued writer cannot skip this call.
        g_activeHookCalls.fetch_add(1, std::memory_order_acq_rel);
        AcquireSRWLockShared(&g_hookLock);
    }
    const bool rendererEnabled = g_acceptRendererCalls;
    ++g_hookCallDepth;
    return rendererEnabled;
}

/** Releases the shared lifetime only after the outermost replacement call finishes. */
__declspec(noinline) void leave_hook_call() noexcept {
    --g_hookCallDepth;
    if (g_hookCallDepth == 0) {
        ReleaseSRWLockShared(&g_hookLock);
        g_activeHookCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
}

/**
 * Appends one optional Sunrise UI frame before the selected swap chain is presented.
 * @param swapChain SDK swap chain supplied by DXGI.
 * @param syncInterval Vertical-sync interval forwarded unchanged.
 * @param flags DXGI presentation flags forwarded unchanged.
 * @return Exact HRESULT returned by the original SDK method.
 */
__declspec(noinline) HRESULT STDMETHODCALLTYPE present_body(IDXGISwapChain* swapChain,
                                                            UINT syncInterval,
                                                            UINT flags) noexcept {
    HRESULT result = DXGI_ERROR_INVALID_CALL;
    const bool rendererEnabled = enter_hook_call();
    const auto call = original<Present>(HookSlot::present);
    if (call != nullptr) {
        if (rendererEnabled && (flags & DXGI_PRESENT_TEST) == 0) {
            // TEST probes presentation status and must not submit overlay draw work.
            renderer::present(swapChain);
        }
        result = call(swapChain, syncInterval, flags);
        if (rendererEnabled) {
            renderer::present_result(swapChain, result);
            // Work that stalls the game waits here for its overlay to reach the screen.
            core::ui::busy::confirm_presented();
        }
    }
    leave_hook_call();
    // Thread-affine capture release runs only after the hook lifetime lock is gone.
    renderer::dispatch_pending_input_release();
    return result;
}

/**
 * Drops and rebuilds the selected render target around a buffer resize.
 * @param swapChain SDK swap chain supplied by DXGI.
 * @return Exact HRESULT returned by the original SDK method.
 */
__declspec(noinline) HRESULT STDMETHODCALLTYPE resize_buffers_body(IDXGISwapChain* swapChain,
                                                                   UINT bufferCount,
                                                                   UINT width,
                                                                   UINT height,
                                                                   DXGI_FORMAT format,
                                                                   UINT flags) noexcept {
    const bool rendererEnabled = enter_hook_call();
    const auto call = original<ResizeBuffers>(HookSlot::resizeBuffers);
    HRESULT result = DXGI_ERROR_INVALID_CALL;
    if (call != nullptr) {
        if (rendererEnabled) {
            renderer::before_surface_change(swapChain);
        }
        result = call(swapChain, bufferCount, width, height, format, flags);
        if (rendererEnabled) {
            renderer::after_surface_change(swapChain, result);
        }
    }
    leave_hook_call();
    return result;
}

/**
 * Drops and rebuilds the selected render target around a mode change.
 * The change recreates the back buffers, so our view must not hold one across it.
 * @param swapChain SDK swap chain supplied by DXGI.
 * @return Exact HRESULT returned by the original SDK method.
 */
__declspec(noinline) HRESULT STDMETHODCALLTYPE set_fullscreen_state_body(
    IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* target) noexcept {
    const bool rendererEnabled = enter_hook_call();
    const auto call = original<SetFullscreenState>(HookSlot::setFullscreenState);
    HRESULT result = DXGI_ERROR_INVALID_CALL;
    if (call != nullptr) {
        if (rendererEnabled) {
            renderer::before_surface_change(swapChain);
        }
        result = call(swapChain, fullscreen, target);
        if (rendererEnabled) {
            renderer::after_surface_change(swapChain, result);
        }
    }
    leave_hook_call();
    return result;
}

} // namespace

/** @return Direct internal-linkage bodies that cannot become linker thunks. */
EntryPoints entry_points() noexcept {
    return {reinterpret_cast<void*>(&present_body),
            reinterpret_cast<void*>(&resize_buffers_body),
            reinterpret_cast<void*>(&set_fullscreen_state_body)};
}

/** @return Every unwind-backed body that can own an admitted replacement call. */
ProtectedEntries protected_entries() noexcept {
    return {
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&present_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&resize_buffers_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&set_fullscreen_state_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&enter_hook_call)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&leave_hook_call)},
    };
}

HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* swapChain,
                                  UINT syncInterval,
                                  UINT flags) noexcept {
    return present_body(swapChain, syncInterval, flags);
}

} // namespace sunrise::client::hooks::graphics::replacement
