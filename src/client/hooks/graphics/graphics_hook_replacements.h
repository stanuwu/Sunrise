#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <d3d11.h>
#include <dxgi.h>

#include "../../hooking/detour.h"
#include "input/input.h"
#include "renderer/renderer.h"

namespace sunrise::client::hooks::graphics {

/** SDK ABI for IDXGISwapChain::Present. */
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
/** SDK ABI for IDXGISwapChain::ResizeBuffers. */
using ResizeBuffers =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
/** SDK ABI for IDXGISwapChain::SetFullscreenState. */
using SetFullscreenState = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

/** Stable slots shared by discovery targets and Detours handles. */
enum class HookSlot : std::size_t {
    present,
    resizeBuffers,
    setFullscreenState,
    count,
};

/** Every SDK method that forms the graphics hook transaction. */
inline constexpr std::size_t kHandleCount = static_cast<std::size_t>(HookSlot::count);
/** IDXGISwapChain inherits 7 methods before its own Present. */
inline constexpr std::size_t kPresentMethodIndex = 8;
/** ResizeBuffers comes 5 IDXGISwapChain methods after Present. */
inline constexpr std::size_t kResizeBuffersMethodIndex = 13;
/** SetFullscreenState comes 2 IDXGISwapChain methods after Present. */
inline constexpr std::size_t kSetFullscreenStateMethodIndex = 10;

/** System methods and module references kept through detachment. */
struct Targets {
    void* present{};
    void* resizeBuffers{};
    void* setFullscreenState{};
    HMODULE d3d11Module{};
    HMODULE dxgiModule{};
};

extern SRWLOCK g_hookLock;
/** Outermost replacement calls counted before they can wait behind hook teardown. */
extern std::atomic_uint g_activeHookCalls;
/** Per-thread depth stops an SRW upgrade from inside an original SDK call chain. */
extern thread_local std::size_t g_hookCallDepth;
/** Shared-lock state that allows renderer work only while both hooks are attached. */
extern bool g_acceptRendererCalls;

/** @return True only when no outer replacement call can still use retained hook state. */
[[nodiscard]] inline bool hook_calls_idle() noexcept {
    return g_activeHookCalls.load(std::memory_order_acquire) == 0;
}

/** @return True while the current thread is inside any graphics replacement call. */
[[nodiscard]] inline bool current_thread_in_hook_call() noexcept {
    return g_hookCallDepth != 0;
}

extern std::array<hooking::detour::Handle, kHandleCount> g_handles;
extern std::array<void*, kHandleCount> g_targetEntries;
extern Targets g_targets;

/**
 * Checks that one callable address belongs to executable memory in an exact image.
 * @param target Candidate function address.
 * @param module Required image allocation base.
 * @return True only for committed executable image memory owned by the module.
 */
[[nodiscard]] bool executable_image_target(const void* target, HMODULE module) noexcept;

/**
 * Returns the active trampoline or the restored target during detachment.
 * @tparam Function Exact SDK method ABI.
 * @return Callable entry for the current attach state.
 */
template <typename Function> [[nodiscard]] Function original(HookSlot slot) noexcept {
    const std::size_t index = static_cast<std::size_t>(slot);
    void* entry = g_handles[index].original;
    if (entry == nullptr) {
        entry = g_targetEntries[index];
    }
    return reinterpret_cast<Function>(entry);
}

namespace discovery {

/** Finds the system DXGI method targets through one temporary SDK swap chain. */
[[nodiscard]] bool resolve(Targets& output) noexcept;

/** Releases retained system-module references and clears the target set. */
void release(Targets& targets) noexcept;

} // namespace discovery

namespace replacement {

/** Every replacement body, plus ingress and egress: the bodies a suspended call can sit in. */
inline constexpr std::size_t kProtectedEntryCount = 5;

/** Direct replacement bodies indexed by HookSlot. */
using EntryPoints = std::array<void*, kHandleCount>;
/** Every unwind-backed body that can own a replacement call during removal. */
using ProtectedEntries = std::array<hooking::detour::ProtectedCodeEntry, kProtectedEntryCount>;

/** @return Direct replacement bodies formed in their owning translation unit. */
[[nodiscard]] EntryPoints entry_points() noexcept;

/** @return Unwind-backed call-lifetime bodies needed for safe detachment. */
[[nodiscard]] ProtectedEntries protected_entries() noexcept;

/** SDK-compatible IDXGISwapChain::Present detour. */
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* swapChain,
                                  UINT syncInterval,
                                  UINT flags) noexcept;

} // namespace replacement

} // namespace sunrise::client::hooks::graphics
