#include "bubble_authority_replacements.h"

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../coordinator/network_call_coordinator.h"
#include "../platform.h"
#include "scope/bubble_authority_scope.h"

namespace sunrise::client::hooks::network::bubble_authority {
namespace {

/** Log the decoder and the forced arm once each. Both run on every roster message. */
std::atomic_bool g_decoderSeen{false};
std::atomic_bool g_forcedSeen{false};

/**
 * Logs the first roster decode and the first forced authority read. The forced read is what makes
 * message 5's per-bubble authority arm apply at all. Without it the client keeps its own build
 * state and only the unusable bubble applies.
 * @param stage Which event this is.
 */
void report_once(const char* stage) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=bubbleauth stage=%s result=ok", stage);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

using Decoder = bool(__fastcall*)(void*, void*, void*);
using ContentUntracked = bool(__fastcall*)();

/**
 * Runs the native roster-prefix decoder inside one thread-local authority scope.
 * @param roster Client roster container borrowed by the native decoder.
 * @param bitStream Native bit reader borrowed for this call.
 * @param event Native activity message storage borrowed for this call.
 * @return The native decoder result, or false when there is no original to call.
 */
__declspec(noinline) bool __fastcall decoder_body(void* roster,
                                                  void* bitStream,
                                                  void* event) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::bubbleAuthorityDecoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Decoder>(lease.original);
    const bool scoped = lease.accepting && call != nullptr;
    bool result{};
    if (scoped) {
        scope::enter();
        if (!g_decoderSeen.exchange(true, std::memory_order_relaxed)) {
            report_once("decode");
        }
    }
    __try {
        if (call != nullptr) {
            result = call(roster, bitStream, event);
        }
    } __finally {
        if (scoped) {
            scope::leave();
        }
        coordinator::g_callEgress();
    }
    return result;
}

/**
 * Keeps the native build-state read, and forces it true only on the scoped decoder thread.
 * @return Native state, or true only inside an admitted authority decoder call.
 */
__declspec(noinline) bool __fastcall content_untracked_body() noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::contentUntrackedGetter, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ContentUntracked>(lease.original);
    bool result{};
    __try {
        if (call != nullptr) {
            result = call();
        }
        const bool forced = !result && scope::active();
        result = result || forced;
        if (forced && !g_forcedSeen.exchange(true, std::memory_order_relaxed)) {
            report_once("force");
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

/** @return The internal-linkage decoder body, kept safe while the detour is removed. */
void* decoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&decoder_body);
}

/** @return The internal-linkage getter body, kept safe while the detour is removed. */
void* content_untracked_entry_point() noexcept {
    return reinterpret_cast<void*>(&content_untracked_body);
}

} // namespace sunrise::client::hooks::network::bubble_authority
