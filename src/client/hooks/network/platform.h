#pragma once

#include <Windows.h>

#include <WinSock2.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../../hooking/detour.h"
#include "../../network/consumer.h"

namespace sunrise::client::hooks::network {

/** Stable slot order shared by target entries and Detours handles. */
enum class HookSlot : std::size_t {
    transportKind,
    authenticationStatus,
    setCertificate,
    httpExecuteRequest,
    bubbleAuthorityDecoder,
    contentUntrackedGetter,
    signOnReadinessFailure,
    signOnReadinessReady,
    count,
};

/** The handle-array size follows the hook-slot enum. */
inline constexpr std::size_t kHandleCount = static_cast<std::size_t>(HookSlot::count);

extern SRWLOCK g_lock;
extern std::array<hooking::detour::Handle, kHandleCount> g_handles;
extern std::array<void*, kHandleCount> g_targetEntries;
extern std::array<bool, kHandleCount> g_accepting;
extern std::atomic<sunrise::client::network::HttpConsumer> g_httpConsumer;
extern std::atomic<sunrise::client::network::BapConsumer> g_bapConsumer;
extern volatile LONG g_activeCalls;

/**
 * Gives the live trampoline, or the restored target while detaching.
 * @tparam Function Exact hook ABI function-pointer type.
 * @param slot Slot in the handle and target-entry arrays.
 * @return Callable entry for the current attach or detach state.
 */
template <typename Function> [[nodiscard]] Function original(HookSlot slot) noexcept {
    const auto index = static_cast<std::size_t>(slot);
    void* entry = g_handles[index].original;
    if (entry == nullptr) {
        entry = g_targetEntries[index];
    }
    return reinterpret_cast<Function>(entry);
}

namespace platform {

/** @return The transport-kind replacement body, from the file that owns it. */
[[nodiscard]] void* transport_kind_entry_point() noexcept;

/** @return The authentication-status replacement body, from the file that owns it. */
[[nodiscard]] void* authentication_status_entry_point() noexcept;

/** @return The certificate replacement body, from the file that owns it. */
[[nodiscard]] void* set_certificate_entry_point() noexcept;

} // namespace platform

namespace http {

/** @return The HTTP executor replacement body, from the file that owns it. */
[[nodiscard]] void* execute_request_entry_point() noexcept;

} // namespace http

namespace signon {

/** @return The pre-SignOn readiness replacement body, from the file that owns it. */
[[nodiscard]] void* readiness_entry_point() noexcept;

/** @return The pre-SignOn ready replacement body, from the file that owns it. */
[[nodiscard]] void* ready_entry_point() noexcept;

} // namespace signon

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Makes the next base-game rollback be skipped, for the lifecycle test. */
void fail_next_game_rollback() noexcept;

/** @return True once, when the lifecycle test armed a rollback failure. */
[[nodiscard]] bool consume_game_rollback_failure() noexcept;

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network
