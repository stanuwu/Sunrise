#include <Windows.h>

#include <array>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "../../../../core/ui/busy/busy.h"
#include "../../../../core/ui/fonts/runtime/ui_runtime_font_lifecycle.h"
#include "../../../../core/ui/layout/layout.h"
#include "../../../../core/ui/notice/ui_notice_overlay.h"
#include "../../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../../core/ui/theme/sunrise_ui_theme.h"
#include "../input/input.h"
#include "graphics_renderer_report.h"
#include "state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
                                                             UINT message,
                                                             WPARAM word,
                                                             LPARAM value);

namespace sunrise::client::hooks::graphics::renderer {
namespace {

/** The overlay uses one render target, set only while Dear ImGui draws. */
constexpr UINT kOverlayRenderTargetCount = 1;
// A visible UI eats this whole range. A range that stopped short of the wheel or the extra
// buttons would leak them to the game with no other sign.
static_assert(WM_MOUSEWHEEL <= WM_MOUSELAST);
static_assert(WM_MOUSEHWHEEL <= WM_MOUSELAST);
static_assert(WM_XBUTTONDBLCLK <= WM_MOUSELAST);

/** @param message Win32 message ID. @return True for ordinary client-area mouse input. */
[[nodiscard]] bool is_mouse_input(UINT message) noexcept {
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}

/** @param message Win32 message ID. @return True for the raw mouse and keyboard delivery. */
[[nodiscard]] bool is_raw_input(UINT message) noexcept {
    return message == WM_INPUT;
}

/**
 * Says which keyboard and text messages the UI bridge takes.
 * @param message Win32 message ID.
 * @return True for ordinary keyboard or text input.
 */
[[nodiscard]] bool is_keyboard_input(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return true;
    default:
        return false;
    }
}

/**
 * Applies a visibility change to Dear ImGui input and drops events queued while hidden.
 * @param visible Current Core visibility state.
 */
void transition_input_visibility_locked(bool visible) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    // The game renders a virtual cursor, so visible Sunrise frames need ImGui's software cursor.
    io.MouseDrawCursor = visible;
    if (visible) {
        g_resources.inputVisible = true;
        return;
    }
    if (!g_resources.inputVisible) {
        return;
    }

    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    g_resources.inputVisible = false;
    // The capture release is deferred because ReleaseCapture can re-enter WndProc at once.
    g_captureReleaseWindow = g_resources.window;
}

/**
 * Draws Dear ImGui data, then puts back every output-merger target that was set before.
 * @param drawData Completed frame draw data.
 */
void draw_data(ImDrawData* drawData) noexcept {
    if (drawData == nullptr) {
        return;
    }
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> priorTargets{};
    ID3D11DepthStencilView* priorDepth = nullptr;
    g_resources.context->OMGetRenderTargets(
        static_cast<UINT>(priorTargets.size()), priorTargets.data(), &priorDepth);

    ID3D11RenderTargetView* overlayTarget = g_resources.renderTarget;
    g_resources.context->OMSetRenderTargets(kOverlayRenderTargetCount, &overlayTarget, nullptr);
    ImGui_ImplDX11_RenderDrawData(drawData);
    g_resources.context->OMSetRenderTargets(
        static_cast<UINT>(priorTargets.size()), priorTargets.data(), priorDepth);

    for (ID3D11RenderTargetView* target : priorTargets) {
        if (target != nullptr) {
            target->Release();
        }
    }
    if (priorDepth != nullptr) {
        priorDepth->Release();
    }
}

} // namespace

/**
 * Runs one Dear ImGui frame. Draw data is sent only while the Core UI is visible.
 */
void render_frame_locked() noexcept {
    if (!fully_active_locked()) {
        return;
    }
    if (core::ui::scaling::dpi::update(g_resources.window)) {
        // Style and text scale change together, before the backend sets up the frame.
        core::ui::theme::apply();
        if (!core::ui::fonts::runtime::apply_scale(core::ui::scaling::dpi::current())) {
            report::note(report::Stage::frame, report::Reason::fontScale);
            (void)shutdown_locked();
            return;
        }
    }
    const core::ui::runtime::VisibilitySnapshot visibility = core::ui::runtime::snapshot();
    transition_input_visibility_locked(visibility.visible);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // A hidden surface still draws until its close animation ends, so the layout decides. The
    // running-work overlay draws whether the surface is open or not.
    const bool surfaceDrawn = core::ui::layout::render(visibility.visible);
    const bool busyDrawn = core::ui::busy::draw();
    const bool noticeDrawn = core::ui::notice::draw();
    if (!surfaceDrawn && !busyDrawn && !noticeDrawn) {
        // A frame nobody claimed still drains backend state, and sends no draw data.
        ImGui::EndFrame();
        return;
    }
    ImGui::Render();
    draw_data(ImGui::GetDrawData());
}

/** Feeds one ordinary window message into the active Dear ImGui context. */
bool handle_window_message(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    if (!fully_active_locked() || g_resources.window != window) {
        ReleaseSRWLockExclusive(&g_rendererLock);
        return false;
    }

    const core::ui::runtime::VisibilitySnapshot visibility = core::ui::runtime::snapshot();
    transition_input_visibility_locked(visibility.visible);
    if (!visibility.visible) {
        // Hidden input stays with the game and never enters Dear ImGui's event queue.
        ReleaseSRWLockExclusive(&g_rendererLock);
        return false;
    }

    (void)ImGui_ImplWin32_WndProcHandler(window, message, word, value);
    // A visible UI is modal: no mouse, keyboard or raw input reaches the game, hit test aside.
    const bool capture =
        is_mouse_input(message) || is_keyboard_input(message) || is_raw_input(message);
    ReleaseSRWLockExclusive(&g_rendererLock);
    return capture;
}

/** Releases mouse capture, but only after the renderer and window locks are gone. */
void dispatch_pending_input_release(HWND window) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    const bool releaseCapture = g_captureReleaseWindow == window && windowThread != 0
                                && windowThread == GetCurrentThreadId();
    if (releaseCapture) {
        g_captureReleaseWindow = nullptr;
    }
    ReleaseSRWLockExclusive(&g_rendererLock);

    if (releaseCapture && GetCapture() == window) {
        // The renderer lock is gone first, because Windows sends WM_CAPTURECHANGED at once.
        (void)ReleaseCapture();
    }
}

/** Runs any deferred capture release once the hook and renderer locks are gone. */
void dispatch_pending_input_release() noexcept {
    AcquireSRWLockShared(&g_rendererLock);
    const HWND window = g_captureReleaseWindow;
    ReleaseSRWLockShared(&g_rendererLock);

    if (window == nullptr) {
        return;
    }
    const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    if (windowThread != 0 && windowThread != GetCurrentThreadId()) {
        // The game's presentation message still reaches hooked Present, even if we were unhooked.
        (void)PostMessageW(window, input::kRequiredForwardMessage, 0, 0);
        return;
    }
    dispatch_pending_input_release(window);
}

} // namespace sunrise::client::hooks::graphics::renderer
