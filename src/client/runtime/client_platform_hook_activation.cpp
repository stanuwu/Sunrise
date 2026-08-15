#include <Windows.h>

#include <array>
#include <span>

#include "../../core/logging/log.h"
#include "../executable/image.h"
#include "../hooks/network/runtime.h"
#include "../patterns/registry.h"
#include "../targets/steam_targets.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::runtime {
namespace {

/**
 * Adds one owned reference to the exact loaded module selected by the Steam boundary.
 * @param module Borrowed module handle.
 * @param retained Receives the owned module reference.
 * @return True when Windows resolved the same loaded image from its base address.
 */
[[nodiscard]] bool retain_module(HMODULE module, HMODULE& retained) noexcept {
    retained = nullptr;
    if (module == nullptr
        || GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &retained)
               == FALSE) {
        return false;
    }
    if (retained == module) {
        return true;
    }
    (void)FreeLibrary(retained);
    retained = nullptr;
    return false;
}

} // namespace
} // namespace sunrise::client::runtime

namespace sunrise::client {

/** Resolves targets in one loaded Steam networking image and installs its hook group once. */
bool activate_platform_once(HMODULE networkingModule) noexcept {
    AcquireSRWLockExclusive(&runtime::g_lock);
    if (runtime::g_platformStage != runtime::StageState::pending) {
        const bool active = runtime::g_platformStage == runtime::StageState::active;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return active;
    }

    HMODULE retainedModule{};
    executable::ExecutableImage networking;
    std::array<patterns::ImageRange, executable::kPeSectionLimit> networkingRanges{};
    if (!runtime::retain_module(networkingModule, retainedModule)
        || !executable::inspect(reinterpret_cast<std::byte*>(retainedModule), networking)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=steam_image result=fail");
        if (retainedModule != nullptr) {
            (void)FreeLibrary(retainedModule);
        }
        runtime::g_platformStage = runtime::StageState::failed;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    for (std::size_t index = 0; index < networking.count; ++index) {
        networkingRanges[index] = patterns::ImageRange{networking.sections[index]};
    }
    if (!targets::steam::resolve(std::span(networkingRanges.data(), networking.count))) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=steam_targets result=fail");
        (void)FreeLibrary(retainedModule);
        runtime::g_platformStage = runtime::StageState::failed;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    if (!hooks::network::install_platform()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=activate stage=platform_hooks result=fail");
        targets::steam::clear();
        (void)FreeLibrary(retainedModule);
        runtime::g_platformStage = runtime::StageState::failed;
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }

    runtime::g_platformModule = retainedModule;
    runtime::g_platformStage = runtime::StageState::active;
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=activate stage=platform result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
