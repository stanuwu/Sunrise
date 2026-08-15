#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../graphics_hook_replacements.h"

namespace sunrise::client::hooks::graphics {
namespace {

/** A 64-pixel hidden surface is enough to get the system DXGI vtable. */
constexpr UINT kProbeExtent = 64;
/** The legacy discard model accepts one buffer for the temporary windowed swap chain. */
constexpr UINT kProbeBufferCount = 1;
/** One sample avoids multisample needs during target discovery. */
constexpr UINT kProbeSampleCount = 1;
/** The low protection byte holds the PAGE_* base mode. */
constexpr DWORD kProtectionBaseMask = 0xFFU;

/** Temporary SDK objects released before production hooks attach. */
struct Probe {
    HWND window{};
    IDXGISwapChain* swapChain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
};

/** @param probe Temporary COM objects released before a fallback creation attempt. */
void release_probe_com(Probe& probe) noexcept {
    if (probe.context != nullptr) {
        probe.context->Release();
        probe.context = nullptr;
    }
    if (probe.swapChain != nullptr) {
        probe.swapChain->Release();
        probe.swapChain = nullptr;
    }
    if (probe.device != nullptr) {
        probe.device->Release();
        probe.device = nullptr;
    }
}

/** @param probe Every temporary SDK object released in dependency order. */
void release_probe(Probe& probe) noexcept {
    release_probe_com(probe);
    if (probe.window != nullptr) {
        DestroyWindow(probe.window);
    }
    probe = {};
}

/**
 * Levels down to the D3D11 floor. The vtable is the same at each, so the probe takes whatever
 * the installed adapter supports.
 */
constexpr std::array kFeatureLevels{
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1,
};

/** One probe creation attempt, named for the log line it produces. */
struct Attempt {
    D3D_DRIVER_TYPE driver{};
    bool requestLevels{};
    std::string_view name{};
};

/**
 * Attempt order. The second asks for no level list, which some older drivers need.
 * WARP is last because it is the slowest to create.
 */
constexpr std::array<Attempt, 3> kAttempts{
    Attempt{D3D_DRIVER_TYPE_HARDWARE, true, "hardware"},
    Attempt{D3D_DRIVER_TYPE_HARDWARE, false, "hardware_default"},
    Attempt{D3D_DRIVER_TYPE_WARP, true, "warp"},
};

/** @param level Level the probe device reached. @param name Attempt that created it. */
void report_probe(std::string_view name, D3D_FEATURE_LEVEL level) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=graphics stage=probe result=ok driver=%.*s level=0x%04X",
                                      static_cast<int>(name.size()),
                                      name.data(),
                                      static_cast<unsigned>(level));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Creates one SDK swap chain, trying each attempt in order.
 * @param create SDK D3D11 factory entry from the system module.
 * @param probe Receives all temporary COM objects.
 * @return True when a classic IDXGISwapChain vtable is available.
 */
[[nodiscard]] bool create_probe(decltype(&D3D11CreateDeviceAndSwapChain) create,
                                Probe& probe) noexcept {
    probe.window = CreateWindowExW(0,
                                   L"STATIC",
                                   L"",
                                   WS_OVERLAPPED,
                                   0,
                                   0,
                                   static_cast<int>(kProbeExtent),
                                   static_cast<int>(kProbeExtent),
                                   nullptr,
                                   nullptr,
                                   GetModuleHandleW(nullptr),
                                   nullptr);
    if (probe.window == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=graphics stage=probe result=fail reason=window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = kProbeExtent;
    description.BufferDesc.Height = kProbeExtent;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = kProbeSampleCount;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kProbeBufferCount;
    description.OutputWindow = probe.window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    for (const Attempt& attempt : kAttempts) {
        D3D_FEATURE_LEVEL level{};
        const HRESULT result =
            create(nullptr,
                   attempt.driver,
                   nullptr,
                   0,
                   attempt.requestLevels ? kFeatureLevels.data() : nullptr,
                   attempt.requestLevels ? static_cast<UINT>(kFeatureLevels.size()) : 0,
                   D3D11_SDK_VERSION,
                   &description,
                   &probe.swapChain,
                   &probe.device,
                   &level,
                   &probe.context);
        if (SUCCEEDED(result) && probe.swapChain != nullptr && probe.device != nullptr
            && probe.context != nullptr) {
            report_probe(attempt.name, level);
            return true;
        }
        release_probe_com(probe);
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::warn,
                     "ev=graphics stage=probe result=fail reason=device");
    return false;
}

} // namespace

/** Checks that one callable address belongs to executable memory in an exact image. */
bool executable_image_target(const void* target, HMODULE module) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (target == nullptr || module == nullptr
        || VirtualQuery(target, &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != module || memory.State != MEM_COMMIT
        || memory.Type != MEM_IMAGE) {
        return false;
    }
    const DWORD protection = memory.Protect & kProtectionBaseMask;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
           || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

namespace discovery {

/** Finds the system DXGI targets and keeps no temporary swap chain. */
bool resolve(Targets& output) noexcept {
    output = {};
    HMODULE dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d11Module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dxgiModule == nullptr || d3d11Module == nullptr) {
        if (d3d11Module != nullptr) {
            FreeLibrary(d3d11Module);
        }
        if (dxgiModule != nullptr) {
            FreeLibrary(dxgiModule);
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=graphics stage=probe result=fail reason=module");
        return false;
    }

    const auto create = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
        GetProcAddress(d3d11Module, "D3D11CreateDeviceAndSwapChain"));
    Probe probe;
    if (create == nullptr || !create_probe(create, probe)) {
        release_probe(probe);
        FreeLibrary(d3d11Module);
        FreeLibrary(dxgiModule);
        return false;
    }

    void** methods = *reinterpret_cast<void***>(probe.swapChain);
    void* present = methods[kPresentMethodIndex];
    void* resizeBuffers = methods[kResizeBuffersMethodIndex];
    void* setFullscreenState = methods[kSetFullscreenStateMethodIndex];
    // Distinct entries prove the slots were read from a real vtable, not from a shared thunk.
    const bool valid = executable_image_target(present, dxgiModule)
                       && executable_image_target(resizeBuffers, dxgiModule)
                       && executable_image_target(setFullscreenState, dxgiModule)
                       && present != resizeBuffers && present != setFullscreenState
                       && resizeBuffers != setFullscreenState;
    release_probe(probe);
    if (!valid) {
        FreeLibrary(d3d11Module);
        FreeLibrary(dxgiModule);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=graphics stage=probe result=fail reason=vtable");
        return false;
    }

    output = Targets{present, resizeBuffers, setFullscreenState, d3d11Module, dxgiModule};
    return true;
}

/** @param targets Retained system modules released after both hooks detach. */
void release(Targets& targets) noexcept {
    if (targets.d3d11Module != nullptr) {
        FreeLibrary(targets.d3d11Module);
    }
    if (targets.dxgiModule != nullptr) {
        FreeLibrary(targets.dxgiModule);
    }
    targets = {};
}

} // namespace discovery
} // namespace sunrise::client::hooks::graphics
