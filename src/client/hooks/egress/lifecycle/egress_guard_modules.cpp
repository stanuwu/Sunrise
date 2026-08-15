#include "internal.h"

namespace sunrise::client::hooks::egress::lifecycle {
namespace {

/** System32 names stop the guard finding exports in the game directory. */
constexpr std::array<const wchar_t*, kModuleCount> kModuleNames{
    L"ws2_32.dll",
    L"mswsock.dll",
    L"dnsapi.dll",
};

std::array<HMODULE, kModuleCount> g_modules{};

} // namespace

/** Releases system modules only while no detour can still target their exports. */
void release_modules() noexcept {
    for (HMODULE& module : g_modules) {
        if (module != nullptr) {
            (void)FreeLibrary(module);
            module = nullptr;
        }
    }
}

/** @return True when every guard dependency is loaded from System32. */
bool load_modules() noexcept {
    for (std::size_t index = 0; index < g_modules.size(); ++index) {
        g_modules[index] =
            LoadLibraryExW(kModuleNames[index], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g_modules[index] == nullptr) {
            release_modules();
            return false;
        }
    }
    return true;
}

/** @return Loaded module, or null before load_modules. */
HMODULE module_handle(ModuleSlot module) noexcept {
    const auto index = static_cast<std::size_t>(module);
    return index < g_modules.size() ? g_modules[index] : nullptr;
}

} // namespace sunrise::client::hooks::egress::lifecycle
