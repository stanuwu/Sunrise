#include "account_registration.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::account_registration {
namespace {
using Upsert = void(__fastcall*)(std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t,
                                 std::uint64_t);
hooking::detour::Handle hook{};
std::atomic<Upsert> original{};
std::atomic_uint calls{};
std::atomic_bool registered{};
std::atomic<const volatile std::uintptr_t*> friendsManager{};

bool local_identity(std::uint64_t address) noexcept {
    if (address == 0) {
        return false;
    }
    __try {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return *reinterpret_cast<const volatile std::uint64_t*>(address)
               == core::settings::get().steam.user.steamId;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool registered_index(std::uint64_t address) noexcept {
    if (!address) {
        return false;
    }
    __try {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return *reinterpret_cast<const volatile std::int32_t*>(address) >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void __fastcall upsert(std::uint64_t a1,
                                            std::uint64_t a2,
                                            std::uint64_t a3,
                                            std::uint64_t a4,
                                            std::uint64_t a5,
                                            std::uint64_t a6,
                                            std::uint64_t a7,
                                            std::uint64_t a8,
                                            std::uint64_t a9) noexcept {
    calls.fetch_add(1, std::memory_order_acq_rel);
    __try {
        auto target = original.load(std::memory_order_acquire);
        while (target == nullptr) {
            original.wait(nullptr, std::memory_order_acquire);
            target = original.load(std::memory_order_acquire);
        }
        const bool own = !registered.load(std::memory_order_acquire) && local_identity(a4);
        target(a1, a2, a3, a4, a5, a6, a7, a8, a9);
        if (own && registered_index(a9)) {
            registered.store(true, std::memory_order_release);
        }
    } __finally {
        calls.fetch_sub(1, std::memory_order_acq_rel);
    }
}
bool idle() noexcept {
    return calls.load(std::memory_order_acquire) == 0;
}
} // namespace

bool install() noexcept {
    if (original.load(std::memory_order_acquire)) {
        return true;
    }
    using namespace patterns;
    constexpr std::string_view text = "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 10 "
                                      "01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 00 01 00 00 "
                                      "4C 8B AC 24 70 01 00 00 41 83 CC FF";
    constexpr auto pattern = signature<signature_length(text)>(text);
    auto* target = scan_main_image_unique(pattern, "account_registration");
    // Only the supported executable is known to have these layouts, so the unique scan must
    // also land on its verified addresses; any other build refuses instead of detouring.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!target || reinterpret_cast<std::uintptr_t>(target) != base + 0xBE4D00U) {
        return false;
    }
    constexpr std::string_view friendsText =
        "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 5C 24 20 "
        "48 89 44 24 40 E8 ? ? ? ? 31 44 24 40 E8 ? ? ? ? 31 44 24 44 "
        "48 8B 5C 24 40 48 85 DB";
    constexpr auto friendsPattern = signature<signature_length(friendsText)>(friendsText);
    auto* callback = scan_main_image_unique(friendsPattern, "persona_callback");
    if (!callback || reinterpret_cast<std::uintptr_t>(callback) != base + 0xA75E90U) {
        return false;
    }
    auto* manager = resolve_relative(callback + 7, callback + 11);
    if (reinterpret_cast<std::uintptr_t>(manager) != base + 0x27C3DC0U) {
        return false;
    }
    registered.store(false, std::memory_order_release);
    if (!hooking::detour::install({target, reinterpret_cast<void*>(&upsert)}, hook)) {
        return false;
    }
    friendsManager.store(reinterpret_cast<const volatile std::uintptr_t*>(manager),
                         std::memory_order_release);
    original.store(reinterpret_cast<Upsert>(hook.original), std::memory_order_release);
    original.notify_all();
    return true;
}
bool uninstall() noexcept {
    if (!hook.attached) {
        return true;
    }
    const std::array protectedCode{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&upsert)}};
    if (hooking::detour::uninstall(hook, protectedCode, &idle)
        != hooking::detour::UninstallResult::removed) {
        return false;
    }
    original.store(nullptr, std::memory_order_release);
    registered.store(false, std::memory_order_release);
    friendsManager.store(nullptr, std::memory_order_release);
    return true;
}
bool own_entry_registered() noexcept {
    return registered.load(std::memory_order_acquire);
}
bool friends_manager_ready() noexcept {
    const auto* manager = friendsManager.load(std::memory_order_acquire);
    if (!manager) {
        return false;
    }
    __try {
        return *manager != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace sunrise::client::hooks::account_registration
