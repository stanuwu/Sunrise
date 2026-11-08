#pragma once

#include <Windows.h>

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>

namespace sunrise::client::diagnostics::test_input {

/** Posted to the render window: wParam is a virtual key; lParam 1 holds it, 0 frees it. */
inline constexpr UINT kKeyMessage = WM_APP + 0x5E1;
/** Posted to the render window: lParam is the client cursor, x low and y high; -1 clears. */
inline constexpr UINT kCursorMessage = WM_APP + 0x5E2;
/** Posted to the render window: the next presented frame is written to SUNRISE_TEST_SHOT. */
inline constexpr UINT kShotMessage = WM_APP + 0x5E3;
/** Posted to the render window: lParam is pad 0's buttons, then left x and y; wParam right x, y. */
inline constexpr UINT kPadMessage = WM_APP + 0x5E4;
/** A WM_INPUT handle with this mark in bits 16-31 carries mouse buttons, and dx and dy above. */
inline constexpr std::uintptr_t kRawMark = 0x5EED0000, kRawMarkMask = 0xFFFF0000;

/** @return True when this process was started with SUNRISE_TEST_INPUT=1. */
[[nodiscard]] bool enabled() noexcept;

/**
 * In a test process, attaches the foreground, cursor, raw-input and pad guards, so a background
 * run takes only the harness's input. Does nothing in a normal session.
 * @return True when the guards attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every guard install attached. */
void uninstall() noexcept;

/**
 * Applies one posted test message and drops mouse leaves. Off unless the process is a test process.
 * @return True when the message was consumed.
 */
[[nodiscard]] bool handle(HWND window, UINT message, WPARAM word, LPARAM value) noexcept;

/** A posted test mouse message carries this mark in wParam bits 32-47; real ones do not. */
inline constexpr WPARAM kMouseMark = 0x5EED00000000, kMouseMarkMask = 0xFFFF00000000;

/**
 * In a test process, reports a real mouse message for dropping, so the user's own cursor never
 * steers a background run. Strips the mark from a posted test message.
 * @return True when the message is real mouse input to drop.
 */
[[nodiscard]] bool foreign_mouse(UINT message, WPARAM& word) noexcept;

/** @return True in a test process for raw input the test did not post. */
[[nodiscard]] bool foreign_raw_input(LPARAM value) noexcept;

/**
 * Writes the frame about to be presented when a shot was asked for. Runs on the present thread
 * after the interface drew, so the shot shows what the window shows.
 */
void capture(IDXGISwapChain* swapChain,
             ID3D11Device* device,
             ID3D11DeviceContext* context) noexcept;

} // namespace sunrise::client::diagnostics::test_input
