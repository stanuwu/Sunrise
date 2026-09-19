#include "presence_publication.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/social/native_presence.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::presence_publication {
namespace {
using Build = void(__fastcall*)(void*, const std::uint64_t*);
using Cache = const std::byte*(__fastcall*)(const void*);
using Publish = void(__fastcall*)(std::uint64_t);
hooking::detour::Handle g_hook{};
std::atomic<Build> g_original{};
std::atomic_uint32_t g_calls{};
Cache g_cache{};
Publish g_publish{};
const void* g_root{};

// Offsets in the native presence cache record: the selected character, the fireteam block that
// follows it, and the pending publication word further in. Every read below is under SEH because
// the record is not guaranteed to exist when the hook runs.
constexpr std::size_t kCacheCharacterOffset = 0xB18;
constexpr std::size_t kCacheFireteamOffset = 0xB38;
constexpr std::size_t kCachePendingOffset = 0x1FF0;

struct Snapshot {
    const std::byte* cache{};
    std::uint64_t owner{}, character{}, pending{};
    std::array<std::byte, state::social::kNativeFireteamSize> members{};
};

bool snapshot(const std::uint64_t* owner, Snapshot& result) noexcept {
    __try {
        if (!owner || !*owner) {
            return false;
        }
        result.owner = *owner;
        result.cache = g_cache(g_root);
        if (!result.cache) {
            return false;
        }
        std::memcpy(
            &result.character, result.cache + kCacheCharacterOffset, sizeof result.character);
        std::memcpy(
            result.members.data(), result.cache + kCacheFireteamOffset, result.members.size());
        std::memcpy(&result.pending, result.cache + kCachePendingOffset, sizeof result.pending);
        return result.character != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void __fastcall build(void* context, const std::uint64_t* owner) noexcept {
    g_calls.fetch_add(1, std::memory_order_acq_rel);
    auto native = g_original.load(std::memory_order_acquire);
    while (native == nullptr) {
        g_original.wait(nullptr, std::memory_order_acquire);
        native = g_original.load(std::memory_order_acquire);
    }
    Snapshot before, after;
    const bool haveBefore = snapshot(owner, before);
    native(context, owner);
    if (haveBefore && snapshot(owner, after) && before.cache == after.cache
        && before.owner == after.owner && before.character == after.character && after.pending == 0
        && before.members != after.members) {
        // The native periodic builder does not mark row-only changes urgent.
        // Its normal flush stages the completed body before this publication request.
        g_publish(after.owner);
    }
    g_calls.fetch_sub(1, std::memory_order_acq_rel);
}

bool idle() noexcept {
    return g_calls.load(std::memory_order_acquire) == 0;
}
} // namespace

bool install() noexcept {
    if (g_hook.attached) {
        return true;
    }
    using namespace patterns;
    constexpr std::string_view buildText =
        "40 55 41 54 41 57 48 8D AC 24 10 C8 FF FF B8 F0 38 00 00 E8 ? ? ? ? 48 2B E0";
    constexpr std::string_view cacheText =
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 "
        "48 8B 59 10 48 8B F9 48 85 DB 0F 84 70 06 00 00";
    constexpr std::string_view publishText =
        "40 53 48 83 EC 20 48 8B D9 48 8D 0D ? ? ? ? E8 ? ? ? ? "
        "48 85 C0 74 07 48 89 98 F0 1F 00 00 48 83 C4 20 5B C3";
    constexpr auto buildPattern = signature<signature_length(buildText)>(buildText);
    constexpr auto cachePattern = signature<signature_length(cacheText)>(cacheText);
    constexpr auto publishPattern = signature<signature_length(publishText)>(publishText);
    auto* target = scan_main_image_unique(buildPattern, "presence_member_build");
    auto* cache = scan_main_image_unique(cachePattern, "presence_write_cache");
    auto* publish = scan_main_image_unique(publishPattern, "presence_publish_changed");
    if (!target || !cache || !publish) {
        return false;
    }
    g_cache = reinterpret_cast<Cache>(cache);
    g_publish = reinterpret_cast<Publish>(publish);
    // LEA RCX, [RIP + displacement] names the publisher's root object. The pair is that
    // displacement's offset in the matched bytes and the next instruction it is relative to.
    g_root = resolve_relative(publish + 12, publish + 16);
    if (!hooking::detour::install({target, reinterpret_cast<void*>(&build)}, g_hook)) {
        return false;
    }
    g_original.store(reinterpret_cast<Build>(g_hook.original), std::memory_order_release);
    g_original.notify_all();
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=presence_publication stage=install result=ok policy=native_changed_rows");
    return true;
}

bool uninstall() noexcept {
    if (!g_hook.attached) {
        return true;
    }
    const std::array entries{hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&build)}};
    if (hooking::detour::uninstall(g_hook, entries, &idle)
        != hooking::detour::UninstallResult::removed) {
        return false;
    }
    g_original.store(nullptr, std::memory_order_release);
    g_cache = nullptr;
    g_publish = nullptr;
    g_root = nullptr;
    return true;
}
} // namespace sunrise::client::hooks::presence_publication
