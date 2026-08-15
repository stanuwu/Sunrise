#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../../../hooking/detour.h"
#include "../internal.h"

namespace sunrise::client::hooks::egress::lifecycle {

/** Stable module order holds the system-library references for the process lifetime. */
enum class ModuleSlot : std::size_t {
    winsock,
    extensions,
    dns,
    count,
};

/** Fixed module count follows the complete module-slot enum. */
inline constexpr std::size_t kModuleCount = static_cast<std::size_t>(ModuleSlot::count);

/** Required exports come before the optional raw-query export in slot order. */
inline constexpr std::size_t kRequiredHookCount = static_cast<std::size_t>(HookSlot::dnsQueryRaw);

/** @return True when every guard dependency is loaded from System32. */
[[nodiscard]] bool load_modules() noexcept;

/** Releases system modules only while no detour can still target their exports. */
void release_modules() noexcept;

/** @return Loaded module, or null before load_modules. */
[[nodiscard]] HMODULE module_handle(ModuleSlot module) noexcept;

/**
 * Finds the whole Windows SDK egress surface for one atomic Detours batch.
 * @param specs Receives fixed target and replacement pairs in hook-slot order.
 * @param names Receives each export name so the outcome can be reported once logging exists.
 * @param resolved Receives the per-export result.
 * @param count Receives required exports plus DnsQueryRaw when the OS provides it.
 * @return True when every required system export exists.
 */
[[nodiscard]] bool resolve_specs(std::span<hooking::detour::Spec, kHookCount> specs,
                                 std::span<const char*, kHookCount> names,
                                 std::span<bool, kHookCount> resolved,
                                 std::size_t& count) noexcept;

} // namespace sunrise::client::hooks::egress::lifecycle
