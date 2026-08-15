#include <Windows.h>

#include <array>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "../../../../core/ui/layout/layout.h"
#include "../../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../cursor/runtime.h"
#include "../../polled_input/runtime.h"
#include "../input/input.h"
#include "graphics_renderer_report.h"
#include "state.h"

namespace sunrise::client::hooks::graphics::renderer {
namespace {

/** Buffer 0 is the back buffer the overlay render target uses. */
constexpr UINT kBackBufferIndex = 0;

/**
 * Queues the Win32 capture release. It must run on the window's own thread.
 * @param resources Resources of the teardown attempt in progress.
 * @return True when another thread must run the release before teardown retries.
 */
[[nodiscard]] bool wait_for_capture_release(Resources& resources) noexcept {
    if (resources.window == nullptr) {
        return false;
    }
    if (IsWindow(resources.window) == FALSE) {
        if (g_captureReleaseWindow == resources.window) {
            g_captureReleaseWindow = nullptr;
        }
        return false;
    }

    const DWORD windowThread = GetWindowThreadProcessId(resources.window, nullptr);
    if (windowThread == 0) {
        return false;
    }
    HWND captured = nullptr;
    if (windowThread == GetCurrentThreadId()) {
        captured = GetCapture();
    } else {
        GUITHREADINFO information{};
        information.cbSize = sizeof(information);
        if (GetGUIThreadInfo(windowThread, &information) == FALSE) {
            // Unknown cross-thread state stays pending until a later dispatch retries.
            g_captureReleaseWindow = resources.window;
            return true;
        }
        captured = information.hwndCapture;
    }
    if (captured != resources.window) {
        if (g_captureReleaseWindow == resources.window) {
            g_captureReleaseWindow = nullptr;
        }
        return false;
    }

    g_captureReleaseWindow = resources.window;
    return windowThread != GetCurrentThreadId();
}

/** @param object COM object we own. Released and cleared when set. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** One typeless back-buffer format and the view format that reads it. */
struct ViewFormat {
    DXGI_FORMAT stored{};
    DXGI_FORMAT view{};
};

/** Every typeless format DXGI allows for a back buffer, with the view format for each. */
constexpr std::array<ViewFormat, 6> kTypelessViewFormats{
    ViewFormat{DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM},
    ViewFormat{DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM},
    ViewFormat{DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM},
    ViewFormat{DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM},
    ViewFormat{DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT},
    ViewFormat{DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT},
};

/**
 * Describes the view for a typeless back buffer, which has no format to infer.
 * @param output Receives the description. Untouched for any other format.
 * @return True when an explicit description was written.
 */
[[nodiscard]] bool describe_view(ID3D11Texture2D* backBuffer,
                                 D3D11_RENDER_TARGET_VIEW_DESC& output) noexcept {
    D3D11_TEXTURE2D_DESC texture{};
    backBuffer->GetDesc(&texture);
    for (const ViewFormat& candidate : kTypelessViewFormats) {
        if (candidate.stored != texture.Format) {
            continue;
        }
        output = {};
        output.Format = candidate.view;
        output.ViewDimension = texture.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                            : D3D11_RTV_DIMENSION_TEXTURE2D;
        return true;
    }
    return false;
}

/**
 * Stops the started layers in reverse order, then frees all SDK resources.
 * @param resources Fully or partly started renderer resources.
 * @return False only when a cleanup step must be retried.
 */
[[nodiscard]] bool shutdown_resources(Resources& resources) noexcept {
    if (wait_for_capture_release(resources)) {
        return false;
    }
    if (resources.inputInstalled) {
        if (!input::uninstall_raw_input_window()) {
            return false;
        }
        if (!input::uninstall()) {
            return false;
        }
        resources.inputInstalled = false;
    }
    if (resources.dx11BackendInitialized) {
        ImGui_ImplDX11_Shutdown();
        resources.dx11BackendInitialized = false;
    }
    if (resources.win32BackendInitialized) {
        ImGui_ImplWin32_Shutdown();
        resources.win32BackendInitialized = false;
    }
    if (resources.layoutInitialized) {
        if (!core::ui::layout::shutdown()) {
            return false;
        }
        resources.layoutInitialized = false;
    }
    release_resources(resources);
    return true;
}

/**
 * Frees a failed staged setup, or keeps ownership so a later retry can free it.
 * @param staged Partial resources from the failed attempt.
 * @return Always false, so the caller can return it directly.
 */
[[nodiscard]] bool discard_staged(Resources& staged) noexcept {
    if (!shutdown_resources(staged)) {
        // Ownership is published only so a later shutdown can free it without a leak.
        g_resources = staged;
    }
    return false;
}

/**
 * Starts every presentation layer over one checked resource set that we own.
 * @param swapChain Candidate supplied by Present.
 * @return True when layout, both backends and window input are all active.
 */
[[nodiscard]] bool initialize_locked(IDXGISwapChain* swapChain) noexcept {
    Resources staged;
    if (!selection::acquire(swapChain, staged)) {
        // acquire reports its own step.
        return false;
    }
    if (!core::ui::layout::initialize()) {
        release_resources(staged);
        report::note(report::Stage::init, report::Reason::layout);
        return false;
    }
    staged.layoutInitialized = true;
    if (!ImGui_ImplWin32_Init(staged.window)) {
        report::note(report::Stage::init, report::Reason::win32Backend);
        return discard_staged(staged);
    }
    staged.win32BackendInitialized = true;
    if (!ImGui_ImplDX11_Init(staged.device, staged.context)) {
        report::note(report::Stage::init, report::Reason::dx11Backend);
        return discard_staged(staged);
    }
    staged.dx11BackendInitialized = true;
    if (!input::install(staged.window)) {
        report::note(report::Stage::init, report::Reason::windowInput);
        return discard_staged(staged);
    }
    staged.inputInstalled = true;
    g_resources = staged;
    report::note_active();
    return true;
}

} // namespace

SRWLOCK g_rendererLock{SRWLOCK_INIT};
Resources g_resources{};
HWND g_captureReleaseWindow{};

/**
 * Makes a render target from the chosen swap chain's current back buffer.
 * @param resources Chosen SDK resources.
 * @return True when a back-buffer RTV is owned.
 */
bool create_render_target(Resources& resources) noexcept {
    if (resources.swapChain == nullptr || resources.device == nullptr
        || resources.renderTarget != nullptr) {
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    const HRESULT bufferResult = resources.swapChain->GetBuffer(
        kBackBufferIndex, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(bufferResult) || backBuffer == nullptr) {
        release_com(backBuffer);
        report::note(report::Stage::target, report::Reason::backBuffer);
        return false;
    }
    HRESULT targetResult =
        resources.device->CreateRenderTargetView(backBuffer, nullptr, &resources.renderTarget);
    if (FAILED(targetResult)) {
        // The inferred description covers a typed buffer only.
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        if (describe_view(backBuffer, view)) {
            targetResult = resources.device->CreateRenderTargetView(
                backBuffer, &view, &resources.renderTarget);
        } else {
            report::note(report::Stage::target, report::Reason::viewFormat);
        }
    }
    release_com(backBuffer);
    if (FAILED(targetResult) || resources.renderTarget == nullptr) {
        report::note(report::Stage::target, report::Reason::view);
        return false;
    }
    return true;
}

/** @param resources Chosen resources whose RTV is released. */
void release_render_target(Resources& resources) noexcept {
    release_com(resources.renderTarget);
}

/** @param resources SDK resources freed in an order that respects their dependencies. */
void release_resources(Resources& resources) noexcept {
    release_render_target(resources);
    release_com(resources.context);
    release_com(resources.device);
    release_com(resources.swapChain);
    resources = {};
}

/**
 * Shuts down the resource set that is published now.
 * @return True when every layer is cleared.
 */
bool shutdown_locked() noexcept {
    return shutdown_resources(g_resources);
}

/** @return True when the whole presentation stack is ready for frames and input. */
bool fully_active_locked() noexcept {
    return g_resources.swapChain != nullptr && g_resources.device != nullptr
           && g_resources.context != nullptr && g_resources.renderTarget != nullptr
           && g_resources.layoutInitialized && g_resources.win32BackendInitialized
           && g_resources.dx11BackendInitialized && g_resources.inputInstalled
           && g_resources.activeSurfaceChanges == 0 && input::active(g_resources.window);
}

/** Shuts the whole renderer down under the exclusive state lock. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    const bool released = shutdown_locked();
    ReleaseSRWLockExclusive(&g_rendererLock);
    return released;
}

/** Reads renderer readiness under the shared state lock. */
bool active() noexcept {
    AcquireSRWLockShared(&g_rendererLock);
    const bool initialized = fully_active_locked();
    ReleaseSRWLockShared(&g_rendererLock);
    return initialized;
}

/** Picks one usable swap chain and draws the UI frame on it. */
void present(IDXGISwapChain* swapChain) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    if (g_resources.swapChain != nullptr && g_resources.swapChain != swapChain
        && g_resources.activeSurfaceChanges == 0
        && selection::matches_output_window(swapChain, g_resources.window)) {
        // A new valid surface for the same window replaces the retired chain.
        if (!shutdown_locked()) {
            ReleaseSRWLockExclusive(&g_rendererLock);
            return;
        }
    }
    if (g_resources.swapChain != nullptr && !fully_active_locked()) {
        if (g_resources.activeSurfaceChanges != 0) {
            // Present can run at the same time as the original call, which owns the back buffer.
            ReleaseSRWLockExclusive(&g_rendererLock);
            return;
        }
        report::note(report::Stage::shutdown, report::Reason::surfaceLost);
        if (!shutdown_locked()) {
            ReleaseSRWLockExclusive(&g_rendererLock);
            return;
        }
    }
    if (g_resources.swapChain == nullptr) {
        (void)initialize_locked(swapChain);
    }
    if (g_resources.swapChain == swapChain && fully_active_locked()) {
        render_frame_locked();
    }
    ReleaseSRWLockExclusive(&g_rendererLock);

    // The cursor policy calls Win32, so it runs only after the renderer lock is gone.
    const bool visible = core::ui::runtime::snapshot().visible;
    cursor::apply_visibility(visible);
    polled_input::apply_visibility(visible);
    // The game makes its raw-mouse window during startup, so the first tries find nothing.
    (void)input::install_raw_input_window();
}

} // namespace sunrise::client::hooks::graphics::renderer
