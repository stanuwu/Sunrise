#pragma once

#include <Windows.h>

#include <cstddef>
#include <d3d11.h>
#include <dxgi.h>

namespace sunrise::client::hooks::graphics::renderer {

/** The SDK objects we own and the started presentation layers, for one swap chain. */
struct Resources {
    IDXGISwapChain* swapChain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11RenderTargetView* renderTarget{};
    HWND window{};
    bool layoutInitialized{};
    bool win32BackendInitialized{};
    bool dx11BackendInitialized{};
    bool inputInstalled{};
    bool inputVisible{};
    bool surfaceChangeDeviceLost{};
    std::size_t activeSurfaceChanges{};
};

extern SRWLOCK g_rendererLock;
extern Resources g_resources;
/** Window whose capture release waits until every renderer and hook lock is gone. */
extern HWND g_captureReleaseWindow;

namespace selection {

[[nodiscard]] bool acquire(IDXGISwapChain* swapChain, Resources& output) noexcept;

[[nodiscard]] bool matches_output_window(IDXGISwapChain* swapChain, HWND window) noexcept;

} // namespace selection

/** @param resources Chosen SDK resources. @return True when a back-buffer RTV is owned. */
[[nodiscard]] bool create_render_target(Resources& resources) noexcept;

/** @param resources Chosen resources whose RTV is unbound and released. */
void release_render_target(Resources& resources) noexcept;

/** @param resources SDK resources freed in an order that respects their dependencies. */
void release_resources(Resources& resources) noexcept;

/** @return True when every layer is shut down and cleared. */
[[nodiscard]] bool shutdown_locked() noexcept;

/** @return True when the whole presentation stack is ready for frames and input. */
[[nodiscard]] bool fully_active_locked() noexcept;

/** Runs one Dear ImGui frame. Draws only while the Core UI is visible. */
void render_frame_locked() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer
