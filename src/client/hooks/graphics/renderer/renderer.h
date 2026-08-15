#pragma once

#include <Windows.h>

#include <dxgi.h>

namespace sunrise::client::hooks::graphics::renderer {

/** Draws the UI frame, if any, for a checked swap chain. */
void present(IDXGISwapChain* swapChain) noexcept;

/** Drops our render target before a call that recreates the back buffers. */
void before_surface_change(IDXGISwapChain* swapChain) noexcept;

/** Rebuilds our render state after that call returns. */
void after_surface_change(IDXGISwapChain* swapChain, HRESULT result) noexcept;

/** Frees our resources when Present reports a lost device. */
void present_result(IDXGISwapChain* swapChain, HRESULT result) noexcept;

/** @return True only after every renderer and window resource is freed. */
[[nodiscard]] bool shutdown() noexcept;

/** @return True once a chosen swap chain has started all UI resources. */
[[nodiscard]] bool active() noexcept;

/**
 * Feeds one window message into the live Dear ImGui context.
 * @param message Win32 message ID.
 * @return True for every mouse, keyboard and raw-input message while the UI is visible.
 */
[[nodiscard]] bool
handle_window_message(HWND window, UINT message, WPARAM word, LPARAM value) noexcept;

/** @param window Live output window whose deferred capture release may run. */
void dispatch_pending_input_release(HWND window) noexcept;

/** Runs any deferred capture release once the hook and renderer locks are gone. */
void dispatch_pending_input_release() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer
