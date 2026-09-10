/** Filters the spatial quarantine condition only for the locally controlled object. */

#include "turn_back.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../executable/image.h"
#include "../../hooking/detour.h"
#include "../../movement/movement_settings_store.h"
#include "../../patterns/image_scan.h"
#include "../../player/controlled_object.h"

#pragma intrinsic(_InterlockedIncrement, _InterlockedDecrement, _InterlockedCompareExchange)

namespace sunrise::client::hooks::turn_back {
namespace {

constexpr std::string_view kQuarantineTimerUpdateText =
    "4C 8B DC 53 56 48 81 EC 98 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 50 "
    "44 8B 09 48 8B D9 49 89 6B 18 41 8B C1 C1 F8 0D 41 81 E1 FF 1F 00 00";
constexpr auto kQuarantineTimerUpdate =
    patterns::signature<patterns::signature_length(kQuarantineTimerUpdateText)>(
        kQuarantineTimerUpdateText);

/** Both updater calls must resolve to the same predicate. */
constexpr std::size_t kPredicateBeforeCallOffset = 0x8C;
constexpr std::size_t kPredicateAfterCallOffset = 0x281;
constexpr std::size_t kNearCallLength = 5;
/** These offsets are retained from the supplied game-build-specific implementation. */
constexpr std::size_t kForcedConditionOffset = 0xB9;
constexpr std::size_t kTimerOwnerOffset = 0x2C;

using QuarantineActive = std::uint8_t(__fastcall*)(void*);

hooking::detour::Handle g_quarantineActiveHandle{};
std::byte* g_beforeReturn{};
std::byte* g_afterReturn{};

/** Serializes lifecycle changes and the publication of the original pointer. */
SRWLOCK g_lifecycleLock{SRWLOCK_INIT};

/** Allows filtering after installation; cleared before any removal attempt. */
std::atomic_bool g_accepting{false};
std::atomic_bool g_suppressionReported{false};

/** Counts complete replacement calls, including time spent in the original. */
alignas(sizeof(LONG)) volatile LONG g_activeCalls{};

/** Reports idle state without locking or waiting while transaction threads are suspended. */
[[nodiscard]] bool calls_idle() noexcept {
    return _InterlockedCompareExchange(&g_activeCalls, 0, 0) == 0;
}

/** Logs an installation failure and returns false to its caller. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=turn_back stage=install result=fail reason=%s", reason);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Tests a complete range without adding an unchecked size to the candidate address. */
[[nodiscard]] bool contains_code(const executable::ExecutableImage& image,
                                 std::uintptr_t address,
                                 std::size_t size) noexcept {
    if (size == 0) {
        return false;
    }
    for (std::size_t index = 0; index < image.count; ++index) {
        const auto section = image.sections[index];
        const auto begin = reinterpret_cast<std::uintptr_t>(section.data());
        if (address < begin) {
            continue;
        }
        const std::uintptr_t offset = address - begin;
        if (offset <= section.size() && size <= section.size() - offset) {
            return true;
        }
    }
    return false;
}

/** Adds a call-site offset only when the address and all five bytes are valid. */
[[nodiscard]] std::byte* call_site(const executable::ExecutableImage& image,
                                   std::byte* updater,
                                   std::size_t offset) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(updater);
    constexpr auto limit = (std::numeric_limits<std::uintptr_t>::max)();
    if (updater == nullptr || offset > limit - base) {
        return nullptr;
    }
    const std::uintptr_t address = base + offset;
    if (address > limit - kNearCallLength || !contains_code(image, address, kNearCallLength)) {
        return nullptr;
    }
    return reinterpret_cast<std::byte*>(address);
}

/** Confirms that the target currently lies on a committed executable page of the main image. */
[[nodiscard]] bool executable_target(const executable::ExecutableImage& image,
                                     std::uintptr_t address) noexcept {
    if (!contains_code(image, address, 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof memory)
            != sizeof memory
        || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
           || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

/**
 * Reads one validated E8 instruction and decodes its signed displacement.
 * @param image Executable ranges of the main module.
 * @param call Candidate call-site address.
 * @param reason Receives a diagnostic key on failure.
 * @return Executable target, or nullptr when validation fails.
 */
[[nodiscard]] std::byte* near_call_target(const executable::ExecutableImage& image,
                                          std::byte* call,
                                          const char*& reason) noexcept {
    reason = "call_bounds";
    const auto address = reinterpret_cast<std::uintptr_t>(call);
    constexpr auto limit = (std::numeric_limits<std::uintptr_t>::max)();
    if (call == nullptr || address > limit - kNearCallLength
        || !contains_code(image, address, kNearCallLength)) {
        return nullptr;
    }
    std::array<std::byte, kNearCallLength> bytes{};
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), call, bytes.data(), bytes.size(), &copied) == FALSE
        || copied != bytes.size()) {
        reason = "call_read";
        return nullptr;
    }
    if (bytes[0] != std::byte{0xE8}) {
        reason = "call_opcode";
        return nullptr;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + 1, sizeof displacement);
    std::uintptr_t target = address + kNearCallLength;
    reason = "call_target";
    if (displacement >= 0) {
        const auto distance = static_cast<std::uintptr_t>(displacement);
        if (distance > limit - target) {
            return nullptr;
        }
        target += distance;
    } else {
        const auto distance = static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
        if (distance > target) {
            return nullptr;
        }
        target -= distance;
    }
    return executable_target(image, target) ? reinterpret_cast<std::byte*>(target) : nullptr;
}

/** Matches an expected caller and the full handle of the locally controlled object. */
[[nodiscard]] bool owns_controlled_call(void* timer, void* caller) noexcept {
    if (timer == nullptr || (caller != g_beforeReturn && caller != g_afterReturn)) {
        return false;
    }
    std::uint32_t target = 0xFFFFFFFFU;
    std::uint32_t controlled = 0xFFFFFFFFU;
    std::memcpy(&target, static_cast<const std::byte*>(timer) + kTimerOwnerOffset, sizeof target);
    return client::player::controlled_object::current_handle(controlled) && target == controlled;
}

/** Runs the predicate logic while the outer replacement owns an active-call token. */
__declspec(noinline) std::uint8_t quarantine_active_body(void* timer, void* caller) noexcept {
    // Installation holds the exclusive lock until the trampoline has been published.
    AcquireSRWLockShared(&g_lifecycleLock);
    const auto next = reinterpret_cast<QuarantineActive>(g_quarantineActiveHandle.original);
    ReleaseSRWLockShared(&g_lifecycleLock);

    const bool disabled = client::movement::turn_back_disabled();
    if (!disabled) {
        g_suppressionReported.store(false, std::memory_order_relaxed);
    }
    if (g_accepting.load(std::memory_order_acquire) && disabled
        && owns_controlled_call(timer, caller)) {
        std::uint8_t forced = 0;
        std::memcpy(
            &forced, static_cast<const std::byte*>(timer) + kForcedConditionOffset, sizeof forced);
        if (forced == 0) {
            bool expected = false;
            if (g_suppressionReported.compare_exchange_strong(
                    expected, true, std::memory_order_relaxed, std::memory_order_relaxed)) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 "ev=turn_back stage=predicate result=spatial_ignored");
            }
            return 0;
        }
    }
    return next != nullptr ? next(timer) : 0;
}

/** Keeps the native caller address and a lifetime token around the entire predicate call. */
__declspec(noinline) std::uint8_t __fastcall quarantine_active(void* timer) noexcept {
    (void)_InterlockedIncrement(&g_activeCalls);
    void* const caller = _ReturnAddress();
    const std::uint8_t result = quarantine_active_body(timer, caller);
    (void)_InterlockedDecrement(&g_activeCalls);
    return result;
}

/** Installs the hook while its original pointer is protected by the exclusive lifecycle lock. */
[[nodiscard]] bool install_locked() noexcept {
    if (g_quarantineActiveHandle.attached) {
        return g_accepting.load(std::memory_order_acquire) || fail("stopping");
    }
    if (!client::player::controlled_object::available()) {
        return fail("dependency");
    }
    executable::ExecutableImage image{};
    if (!executable::inspect_main_module(image)) {
        return fail("image");
    }
    std::byte* const updater = patterns::scan_main_image_unique(
        kQuarantineTimerUpdate, "turn_back_quarantine_timer_update");
    if (updater == nullptr) {
        return fail("signature");
    }
    std::byte* const beforeCall = call_site(image, updater, kPredicateBeforeCallOffset);
    std::byte* const afterCall = call_site(image, updater, kPredicateAfterCallOffset);
    if (beforeCall == nullptr || afterCall == nullptr) {
        return fail("call_bounds");
    }
    const char* reason = "call_target";
    std::byte* const beforeTarget = near_call_target(image, beforeCall, reason);
    if (beforeTarget == nullptr) {
        return fail(reason);
    }
    std::byte* const afterTarget = near_call_target(image, afterCall, reason);
    if (afterTarget == nullptr) {
        return fail(reason);
    }
    if (beforeTarget != afterTarget) {
        return fail("call_target");
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{beforeTarget, reinterpret_cast<void*>(&quarantine_active)},
            g_quarantineActiveHandle)) {
        return fail("detour");
    }
    g_beforeReturn = beforeCall + kNearCallLength;
    g_afterReturn = afterCall + kNearCallLength;
    g_suppressionReported.store(false, std::memory_order_relaxed);
    g_accepting.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=turn_back stage=install result=ok mode=spatial_predicate_filter");
    return true;
}

/** Removes the hook without clearing state on a refused transaction. */
[[nodiscard]] bool uninstall_locked() noexcept {
    if (!g_quarantineActiveHandle.attached) {
        return true;
    }
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&quarantine_active)},
    };
    const auto result =
        hooking::detour::uninstall(g_quarantineActiveHandle, protectedEntries, &calls_idle);
    if (result != hooking::detour::UninstallResult::removed) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         result == hooking::detour::UninstallResult::protectedCodeActive
                             ? "ev=turn_back stage=uninstall result=fail reason=active_calls"
                             : "ev=turn_back stage=uninstall result=fail reason=detour");
        return false;
    }
    g_beforeReturn = nullptr;
    g_afterReturn = nullptr;
    g_suppressionReported.store(false, std::memory_order_relaxed);
    return true;
}

} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    const bool installed = install_locked();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return installed;
}

bool available() noexcept {
    return g_accepting.load(std::memory_order_acquire);
}

bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_accepting.store(false, std::memory_order_release);
    const bool removed = uninstall_locked();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return removed;
}

} // namespace sunrise::client::hooks::turn_back
