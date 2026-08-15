#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

using core::log::kLineCapacity;

/**
 * The one call to the status 5-to-6 readiness predicate, in the join-request builder. The
 * predicate's entry is a 5-byte jump with no pattern, so the target comes from this call's own
 * operand. Every displacement is wildcarded, so the match runs on to stay unique.
 */
constexpr std::string_view kCallSignatureText =
    "E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? E8 ? ? ? ? BA 1C 00 00 00 48 8B CE 4C 63 40 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kCallSignature = signature<signature_length(kCallSignatureText)>(kCallSignatureText);

/** Offset of the 4-byte displacement inside the matched call instruction. */
constexpr std::size_t kCallOperandOffset = 1;
/** Length of the matched call instruction. */
constexpr std::size_t kCallLength = 5;

/** Answer that lets the join request advance the session to status 6. */
constexpr bool kReady = true;

using JoinRequestReady = bool(__fastcall*)(void*) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<JoinRequestReady> g_original{nullptr};
std::atomic_bool g_seen{false};
std::atomic_bool g_lastNative{false};

/**
 * Logs the native answer when it changes, so a run shows whether the force was needed.
 * @param native What the predicate answered on its own.
 */
void report(bool native) noexcept {
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=join_ready result=%s",
                                      native ? "native" : "forced");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports the activity session ready to move from status 5 to status 6.
 * The native five-term predicate should pass from activity state, so this force is temporary.
 * The predicate has one call site, so nothing else sees the forced answer.
 * @param client Borrowed activity client; the forced answer does not depend on it.
 * @return The native answer, or ready when the native answer refuses.
 */
__declspec(noinline) bool __fastcall join_request_ready(void* client) noexcept {
    const JoinRequestReady original = g_original.load(std::memory_order_acquire);
    const bool native = original != nullptr && original(client);
    if (g_lastNative.exchange(native, std::memory_order_relaxed) != native
        || !g_seen.exchange(true, std::memory_order_relaxed)) {
        report(native);
    }
    if (native || !core::settings::get().client.forceJoinRequestReady) {
        return native;
    }
    return kReady;
}

} // namespace

/** Attaches the join-request readiness force. */
bool install_join_request_ready() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const call = scan_main_image_unique(kCallSignature, "join_request_ready_call");
    if (call == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=join_ready result=fail reason=target");
        return false;
    }
    std::byte* const target = resolve_relative(call + kCallOperandOffset, call + kCallLength);
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&join_request_ready)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=join_ready result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<JoinRequestReady>(g_handle.original),
                     std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=join_ready result=ok");
    return true;
}

/** Detaches the join-request readiness force. */
void uninstall_join_request_ready() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_seen.store(false, std::memory_order_release);
    g_lastNative.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
