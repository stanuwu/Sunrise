#include "replication_budget.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::replication_budget {
namespace {
using Extract = std::int32_t(__fastcall*)(
    void*, std::uint32_t, void*, std::uint32_t, std::int32_t, std::int32_t);
hooking::detour::Handle g_hook{};
std::atomic<Extract> g_original{};
std::atomic_uint32_t g_calls{};

__declspec(noinline) std::int32_t __fastcall extract_tick(void* manager,
                                                          std::uint32_t frame,
                                                          void* stream,
                                                          std::uint32_t minimumBits,
                                                          std::int32_t softReserve,
                                                          std::int32_t hardReserve) noexcept {
    g_calls.fetch_add(1, std::memory_order_acq_rel);
    auto original = g_original.load(std::memory_order_acquire);
    while (original == nullptr) {
        g_original.wait(nullptr, std::memory_order_acquire);
        original = g_original.load(std::memory_order_acquire);
    }
    // Give actor updates the stream capacity while reserving subsequent packet contents.
    if (hardReserve >= 0 && softReserve > hardReserve) {
        softReserve = hardReserve;
    }
    const auto result = original(manager, frame, stream, minimumBits, softReserve, hardReserve);
    g_calls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

bool idle() noexcept {
    return g_calls.load(std::memory_order_acquire) == 0;
}
} // namespace

bool install() noexcept {
    if (g_original.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    using namespace patterns;
    constexpr std::string_view text =
        "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 B8 80 AA 01 00 E8 ? ? ? ?";
    constexpr auto pattern = signature<signature_length(text)>(text);
    auto* target = scan_main_image_unique(pattern, "replication_extract_budget");
    // The reserve argument order is only known for the supported executable; refuse elsewhere.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (target == nullptr || reinterpret_cast<std::uintptr_t>(target) != base + 0x17B0D70U) {
        return false;
    }
    if (!hooking::detour::install({target, reinterpret_cast<void*>(&extract_tick)}, g_hook)) {
        return false;
    }
    g_original.store(reinterpret_cast<Extract>(g_hook.original), std::memory_order_release);
    g_original.notify_all();
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=replication_budget stage=install result=ok policy=stream_capacity");
    return true;
}

bool uninstall() noexcept {
    if (!g_hook.attached) {
        return true;
    }
    const std::array entries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&extract_tick)}};
    // A thread inside the native callee still owns a return through this trampoline.
    if (hooking::detour::uninstall(g_hook, entries, &idle)
        != hooking::detour::UninstallResult::removed) {
        return false;
    }
    g_original.store(nullptr, std::memory_order_release);
    return true;
}
} // namespace sunrise::client::hooks::replication_budget
