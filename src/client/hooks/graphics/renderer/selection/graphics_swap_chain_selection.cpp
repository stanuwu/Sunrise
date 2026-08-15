#include <Windows.h>

#include <bit>
#include <cstdint>

#include "../../graphics_hook_replacements.h"
#include "../graphics_renderer_report.h"
#include "../state.h"

namespace sunrise::client::hooks::graphics::renderer::selection {
namespace {

/** A 64-pixel minimum rejects hidden probe windows and short-lived zero-area ones. */
constexpr std::int64_t kMinimumClientExtent = 64;

/**
 * Checks the window's identity and size. Does not use a game class name.
 * Each rejection reports its own reason.
 * @param window Swap-chain output window.
 * @return True for a visible root window of this process, on this thread.
 */
[[nodiscard]] bool eligible_window(HWND window, bool reporting) noexcept {
    const auto reject = [reporting](report::Reason reason) noexcept {
        if (reporting) {
            report::note(report::Stage::acquire, reason);
        }
        return false;
    };
    if (window == nullptr || IsWindow(window) == FALSE || IsWindowVisible(window) == FALSE
        || GetAncestor(window, GA_ROOT) != window) {
        return reject(report::Reason::window);
    }

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || threadId != GetCurrentThreadId()) {
        return reject(report::Reason::windowThread);
    }

    RECT client{};
    if (GetClientRect(window, &client) == FALSE || client.right <= client.left
        || client.bottom <= client.top) {
        return reject(report::Reason::window);
    }
    const std::int64_t width = static_cast<std::int64_t>(client.right) - client.left;
    const std::int64_t height = static_cast<std::int64_t>(client.bottom) - client.top;
    if (width < kMinimumClientExtent || height < kMinimumClientExtent) {
        return reject(report::Reason::window);
    }
    return true;
}

/**
 * Checks that the window class procedure lives in the main image.
 * @param window Window that already passed the other checks.
 * @return True when the class can belong to the shipped game executable.
 */
[[nodiscard]] bool main_image_window_class(HWND window, bool reporting) noexcept {
    SetLastError(ERROR_SUCCESS);
    const ULONG_PTR procedure = GetClassLongPtrW(window, GCLP_WNDPROC);
    const bool owned = (procedure != 0 || GetLastError() == ERROR_SUCCESS)
                       && executable_image_target(std::bit_cast<const void*>(procedure),
                                                  GetModuleHandleW(nullptr));
    if (!owned && reporting) {
        report::note(report::Stage::acquire, report::Reason::windowClass);
    }
    return owned;
}

/**
 * Checks the swap-chain description before we take any COM reference.
 * @param window Receives the output window.
 * @param reporting False for a query, so a second surface does not log an acquire failure.
 * @return True when the candidate can be the game's current D3D11 presentation surface.
 */
[[nodiscard]] bool
eligible_swap_chain(IDXGISwapChain* swapChain, HWND& window, bool reporting) noexcept {
    if (swapChain == nullptr) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))
        || (description.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0) {
        if (reporting) {
            report::note(report::Stage::acquire, report::Reason::description);
        }
        return false;
    }
    if (!eligible_window(description.OutputWindow, reporting)
        || !main_image_window_class(description.OutputWindow, reporting)) {
        return false;
    }
    window = description.OutputWindow;
    return true;
}

} // namespace

/**
 * Checks a D3D11 swap chain, then takes a reference on its resource set.
 * @param swapChain Candidate handed to the shared Present detour.
 * @param output Receives the SDK resources, only after the checks pass.
 * @return True when the candidate is the game's current presentation surface.
 */
bool acquire(IDXGISwapChain* swapChain, Resources& output) noexcept {
    output = {};
    HWND window = nullptr;
    if (!eligible_swap_chain(swapChain, window, true)) {
        return false;
    }

    Resources staged;
    staged.window = window;
    const HRESULT deviceResult =
        swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&staged.device));
    if (FAILED(deviceResult) || staged.device == nullptr) {
        // A D3D12 or D3D11On12 surface fails here.
        report::note(report::Stage::acquire, report::Reason::device);
        release_resources(staged);
        return false;
    }
    staged.device->GetImmediateContext(&staged.context);
    if (staged.context == nullptr) {
        report::note(report::Stage::acquire, report::Reason::context);
        release_resources(staged);
        return false;
    }
    swapChain->AddRef();
    staged.swapChain = swapChain;
    if (!create_render_target(staged)) {
        release_resources(staged);
        return false;
    }

    output = staged;
    return true;
}

/**
 * Checks whether a candidate could replace the chain for our chosen output window.
 * @param swapChain Candidate handed to the shared Present detour.
 * @param window Chosen output window, which must stay the same.
 * @return True when the candidate is a different valid surface for that window.
 */
bool matches_output_window(IDXGISwapChain* swapChain, HWND window) noexcept {
    HWND candidateWindow = nullptr;
    return window != nullptr && eligible_swap_chain(swapChain, candidateWindow, false)
           && candidateWindow == window;
}

} // namespace sunrise::client::hooks::graphics::renderer::selection
