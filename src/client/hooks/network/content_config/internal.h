#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../hooking/detour.h"

namespace sunrise::client::hooks::network::content_config {

using ContentFetch = std::int64_t(__fastcall*)(const char*, const char*, char);
using ContentTick = void(__fastcall*)(void*);

/** Stable hook slots for local content routing. */
enum class HookSlot : std::size_t {
    enqueueGet,
    contentTick,
    count,
};

/** Array sizes follow the hook-slot enum. */
inline constexpr std::size_t kHookCount = static_cast<std::size_t>(HookSlot::count);

extern SRWLOCK g_lifecycleLock;
extern std::array<hooking::detour::Handle, kHookCount> g_handles;
extern std::array<void*, kHookCount> g_originals;
extern std::array<void*, kHookCount> g_targetEntries;
extern std::array<bool, kHookCount> g_accepting;
extern void* g_contentFetch;
extern bool g_trackCalls;
extern volatile LONG g_activeCalls;

/** @return The array index for a hook slot. */
[[nodiscard]] constexpr std::size_t index(HookSlot slot) noexcept {
    return static_cast<std::size_t>(slot);
}

namespace request {

/** @return The allocated-buffer GET replacement body, from the file that owns it. */
[[nodiscard]] void* entry_point() noexcept;

} // namespace request

namespace tick {

/** @return The content tick replacement body, from the file that owns it. */
[[nodiscard]] void* entry_point() noexcept;

} // namespace tick

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Makes the next hook install fail, for the lifecycle test. */
void fail_next_install() noexcept;

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network::content_config
