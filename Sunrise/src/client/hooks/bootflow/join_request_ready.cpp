#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/forced/activity_forced_destination.h"
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

/**
 * The decrypted body of the predicate. Its entry is an opaque jump stub, so the callback calls
 * are found independently instead of assuming a fixed distance from that entry.
 */
constexpr std::string_view kTermBodySignatureText =
    "48 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 0F B6 F0 84 C0 74 ? "
    "48 8B 8B 20 6C 00 00 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? "
    "40 38 38 74 ? 0F B6 48 01 84 C9 74 ? 83 78 10 00 7F ? "
    "84 C9 75 ? 40 B7 01 48 8B 03 48 8B CB FF 50 10";
constexpr auto kTermBodySignature =
    signature<signature_length(kTermBodySignatureText)>(kTermBodySignatureText);

/** Offset of the 4-byte displacement inside the matched call instruction. */
constexpr std::size_t kCallOperandOffset = 1;
/** Length of the matched call instruction. */
constexpr std::size_t kCallLength = 5;

/** Answer that lets the join request advance the session to status 6. */
constexpr bool kReady = true;
/** Calls in the native predicate that expose its two callback-derived terms. */
constexpr std::size_t kResolveFirstCall = 0x01;
constexpr std::size_t kFirstTermCall = 0x09;
constexpr std::size_t kResolveRosterCall = 0x1C;
constexpr std::size_t kRosterStateCall = 0x24;
/** The callback opcodes above are direct relative calls. */
constexpr std::byte kRelativeCallOpcode{0xE8};
constexpr std::size_t kRelativeCallOperand = 1;
constexpr std::size_t kRelativeCallLength = 5;
/** Native ActivityClient fields used by the predicate's last two terms. */
constexpr std::size_t kRosterContainerOffset = 0x6C20;
constexpr std::size_t kEntitySlotsReadyOffset = 0x6029A;
constexpr std::size_t kMembershipStatusOffset = 0x131;
constexpr std::uint8_t kMembershipReadyBit = 1;
/** The roster-state callback returns this small readiness record. */
constexpr std::size_t kRosterEnabledOffset = 0;
constexpr std::size_t kRosterModeOffset = 1;
constexpr std::size_t kRosterCountOffset = 0x10;
/** Avoids doubling the native callback work on every polling frame. */
constexpr std::uint64_t kTermProbeIntervalMs = 250;

using JoinRequestReady = bool(__fastcall*)(void*) noexcept;
using ResolveReadinessContext = void*(__fastcall*)(void*) noexcept;
using TestReadinessContext = bool(__fastcall*)(void*) noexcept;
using ResolveRosterState = const std::byte*(__fastcall*)(void*) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<JoinRequestReady> g_original{nullptr};
std::atomic<ResolveReadinessContext> g_resolveReadinessContext{nullptr};
std::atomic<TestReadinessContext> g_testReadinessContext{nullptr};
std::atomic<ResolveRosterState> g_resolveRosterState{nullptr};
std::atomic_bool g_seen{false};
std::atomic_bool g_lastNative{false};
std::atomic_uint8_t g_lastTerms{0xFF};
std::atomic_uint64_t g_nextTermProbe{};

/** One decoded term of the native five-way readiness conjunction. */
enum class ReadinessTerm : std::uint8_t {
    client = 0,
    session = 1,
    roster = 2,
    entitySlots = 3,
    membership = 4,
};

struct ReadinessTerms final {
    void* sessionContext{};
    void* rosterContext{};
    const std::byte* rosterState{};
    std::uint8_t mask{};
};

/** Adds one term to the compact diagnostic mask. */
void set_term(ReadinessTerms& terms, ReadinessTerm term, bool ready) noexcept {
    if (ready) {
        terms.mask |= std::uint8_t{1} << static_cast<std::uint8_t>(term);
    }
}

/** Replays the native predicate's independent getters so a false answer names its missing term. */
[[nodiscard]] bool inspect_terms(void* client, ReadinessTerms& terms) noexcept {
    terms = {};
    const ResolveReadinessContext resolveContext =
        g_resolveReadinessContext.load(std::memory_order_acquire);
    const TestReadinessContext testContext = g_testReadinessContext.load(std::memory_order_acquire);
    const ResolveRosterState resolveRosterState =
        g_resolveRosterState.load(std::memory_order_acquire);
    if (client == nullptr || resolveContext == nullptr || testContext == nullptr
        || resolveRosterState == nullptr) {
        return false;
    }

    terms.sessionContext = resolveContext(client);
    set_term(terms,
             ReadinessTerm::session,
             terms.sessionContext != nullptr && testContext(terms.sessionContext));

    void* const rosterContainer = *reinterpret_cast<void* const*>(
        static_cast<const std::byte*>(client) + kRosterContainerOffset);
    terms.rosterContext = rosterContainer == nullptr ? nullptr : resolveContext(rosterContainer);
    terms.rosterState =
        terms.rosterContext == nullptr ? nullptr : resolveRosterState(terms.rosterContext);
    bool rosterReady = false;
    if (terms.rosterState != nullptr) {
        const auto mode = static_cast<std::uint8_t>(terms.rosterState[kRosterModeOffset]);
        const auto count =
            *reinterpret_cast<const std::int32_t*>(terms.rosterState + kRosterCountOffset);
        rosterReady =
            terms.rosterState[kRosterEnabledOffset] != std::byte{} && (mode == 0 || count > 0);
    }
    set_term(terms, ReadinessTerm::roster, rosterReady);

    void** const vtable = *static_cast<void***>(client);
    const auto testClient =
        vtable == nullptr ? nullptr : reinterpret_cast<TestReadinessContext>(vtable[2]);
    set_term(terms, ReadinessTerm::client, testClient != nullptr && testClient(client));

    const auto* const bytes = static_cast<const std::byte*>(client);
    set_term(terms, ReadinessTerm::entitySlots, bytes[kEntitySlotsReadyOffset] != std::byte{});
    set_term(terms,
             ReadinessTerm::membership,
             (static_cast<std::uint8_t>(bytes[kMembershipStatusOffset]) & kMembershipReadyBit)
                 != 0);
    return true;
}

/** Reports a changed term mask while the native readiness predicate is being polled. */
void report_terms(void* client) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::uint64_t next = g_nextTermProbe.load(std::memory_order_relaxed);
    if (now < next
        || !g_nextTermProbe.compare_exchange_strong(
            next, now + kTermProbeIntervalMs, std::memory_order_relaxed)) {
        return;
    }
    ReadinessTerms terms{};
    if (!inspect_terms(client, terms)) {
        return;
    }
    if (g_lastTerms.exchange(terms.mask, std::memory_order_relaxed) == terms.mask) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=bootflow stage=join_ready_terms mask=0x%02X client=%u session=%u roster=%u "
        "entity_slots=%u membership=%u session_ctx=%p roster_ctx=%p roster_state=%p",
        static_cast<unsigned>(terms.mask),
        (terms.mask & (std::uint8_t{1} << static_cast<std::uint8_t>(ReadinessTerm::client))) != 0,
        (terms.mask & (std::uint8_t{1} << static_cast<std::uint8_t>(ReadinessTerm::session))) != 0,
        (terms.mask & (std::uint8_t{1} << static_cast<std::uint8_t>(ReadinessTerm::roster))) != 0,
        (terms.mask & (std::uint8_t{1} << static_cast<std::uint8_t>(ReadinessTerm::entitySlots)))
            != 0,
        (terms.mask & (std::uint8_t{1} << static_cast<std::uint8_t>(ReadinessTerm::membership)))
            != 0,
        terms.sessionContext,
        terms.rosterContext,
        terms.rosterState);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Resolves and validates the four calls used by the term probe before anything is detoured. */
[[nodiscard]] bool resolve_term_targets() noexcept {
    std::byte* const body = scan_main_image_unique(kTermBodySignature, "join_ready_terms");
    if (body == nullptr || body[kResolveFirstCall] != kRelativeCallOpcode
        || body[kFirstTermCall] != kRelativeCallOpcode
        || body[kResolveRosterCall] != kRelativeCallOpcode
        || body[kRosterStateCall] != kRelativeCallOpcode) {
        return false;
    }
    const auto resolveContext = reinterpret_cast<ResolveReadinessContext>(
        resolve_relative(body + kResolveFirstCall + kRelativeCallOperand,
                         body + kResolveFirstCall + kRelativeCallLength));
    const auto secondResolveContext = reinterpret_cast<ResolveReadinessContext>(
        resolve_relative(body + kResolveRosterCall + kRelativeCallOperand,
                         body + kResolveRosterCall + kRelativeCallLength));
    const auto testContext = reinterpret_cast<TestReadinessContext>(resolve_relative(
        body + kFirstTermCall + kRelativeCallOperand, body + kFirstTermCall + kRelativeCallLength));
    const auto resolveRosterState = reinterpret_cast<ResolveRosterState>(
        resolve_relative(body + kRosterStateCall + kRelativeCallOperand,
                         body + kRosterStateCall + kRelativeCallLength));
    if (resolveContext == nullptr || resolveContext != secondResolveContext
        || testContext == nullptr || resolveRosterState == nullptr) {
        return false;
    }
    g_resolveReadinessContext.store(resolveContext, std::memory_order_release);
    g_testReadinessContext.store(testContext, std::memory_order_release);
    g_resolveRosterState.store(resolveRosterState, std::memory_order_release);
    return true;
}

/**
 * Logs the native answer when it changes, so a run shows whether the force was needed.
 * @param native What the predicate answered on its own.
 */
void report(bool native, bool forced) noexcept {
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=join_ready result=%s",
                                      native   ? "native"
                                      : forced ? "forced"
                                               : "waiting");
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
    const bool forced = !native && core::settings::get().client.forceJoinRequestReady
                        && !state::activity::forced::override_active();
    if (g_lastNative.exchange(native, std::memory_order_relaxed) != native
        || !g_seen.exchange(true, std::memory_order_relaxed)) {
        report(native, forced);
    }
    return native || (forced && kReady);
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
    if (!resolve_term_targets()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=join_ready_terms result=fail reason=layout");
    }
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
    g_resolveReadinessContext.store(nullptr, std::memory_order_release);
    g_testReadinessContext.store(nullptr, std::memory_order_release);
    g_resolveRosterState.store(nullptr, std::memory_order_release);
    g_seen.store(false, std::memory_order_release);
    g_lastNative.store(false, std::memory_order_release);
    g_lastTerms.store(0xFF, std::memory_order_release);
    g_nextTermProbe.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
