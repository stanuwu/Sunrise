/**
 * A read-only probe on the client's activity msg 12 handler.
 * Two runs of a public-target membership body ended on a black screen, and the two explanations
 * left standing contradict each other: the client either processed the body and its world
 * container still failed to bind, or it never processed it at all. The status word the handler
 * writes separates them, and nothing else in reach reports it.
 */

#include "membership_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::membership_probe {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * `ActivityMsg12_ReplicateMembership_Recv` @ `0x7FF7421B7240`.
 * Its first argument is the ActivityClient. It commits the membership block at `+27696`, then
 * sets bit `0x100` of the status word at `+304` unconditionally, before returning 1.
 */
constexpr std::string_view kReceiveText = "40 55 53 41 56 41 57 48 8D AC 24 ? ? ? ? B8 C8 96 05 00";
constexpr auto kReceive = signature<signature_length(kReceiveText)>(kReceiveText);

/** Status word the handler writes. `RE/25 "The +304 status word, mapped"` owns its bits. */
constexpr std::size_t kStatusWordOffset = 304;
/** Membership header. Its leading qword is the member key the client matches itself by. */
constexpr std::size_t kMembershipHeaderOffset = 27696;
/**
 * The two slot axes and the printable label the constructor builds from them.
 * Axis 1 is PRIVATE or PUBLIC, axis 2 is CURRENT or TARGET, and the slot map indexes them as
 * `axis1 + 2 * axis2`. A TARGET slot is index 2 or 3, which the public-first current-slot pick
 * never reads -- so the label says outright which readers can ever see this client.
 */
constexpr std::size_t kSlotAxisOneOffset = 24;
constexpr std::size_t kSlotAxisTwoOffset = 28;
constexpr std::size_t kSlotLabelOffset = 32;
/** The label is NUL-terminated; "PRIVATE CURRENT" is the longest form. */
constexpr std::size_t kSlotLabelCapacity = 32;
/**
 * Established session id, returned by the client's own vtable slot 1.
 * The rebind skips a slot outright when this is zero, with no log and no other symptom. Msg 4's
 * accept arm is its only writer, from `join_request` field 1 inside the `join_result` we send.
 */
constexpr std::size_t kEstablishedSessionOffset = 16352;
/**
 * Slot record this client belongs to. The map's target, laid out as `mgr + 720 * index + 10752`,
 * so the difference between two clients' values divided by 720 is their slot-record index delta.
 */
constexpr std::size_t kSlotRecordOffset = 27672;
/** The slot's roster container. The public-first current-slot pick requires it non-null. */
constexpr std::size_t kRosterContainerOffset = 27680;
/**
 * Sticky bind receipt. Zero from construction, one the moment a world container binds, and back
 * to zero only on a session reset. The grant dirty byte is cleared within a tick, so a sample can
 * miss it; this one cannot be missed.
 */
constexpr std::size_t kBindReceiptOffset = 27689;
/** Bit the handler sets, which the world-container bind and the player watcher both read. */
constexpr std::uint16_t kMembershipFlag = 0x100;
/** Entity-slot grant the client has taken but not yet applied to a view. 8192 bits. */
constexpr std::size_t kPendingMaskOffset = 392856;
constexpr std::size_t kPendingMaskSize = 1024;
/** Set by a world-container bind to re-post a grant that arrived before the bind. */
constexpr std::size_t kGrantDirtyOffset = 393880;
/** How long after a message a client is still sampled. The bind lands well inside this. */
constexpr std::uint64_t kSampleWindowMs = 30'000;
/** Sampling cadence. The bind is a tick, not a timer, so this only has to be finer than the wait.
 */
constexpr std::uint64_t kSampleIntervalMs = 2'000;
/** Clients the probe tracks at once. One private and one public target is the live shape. */
constexpr std::size_t kTrackedCapacity = 4;

using Receive = char(__fastcall*)(std::int64_t, std::int64_t, int);

/** One ActivityClient seen carrying a membership body, sampled until its window closes. */
struct Tracked {
    std::int64_t client{};
    std::uint64_t expiresAt{};
    std::uint64_t nextSample{};
    bool occupied{};
};

hooking::detour::Handle g_handle{};
std::atomic_bool g_installed{false};
/** The detour runs on the client's network thread and the sampler on the callback pump. */
SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Tracked, kTrackedCapacity> g_tracked{};

/**
 * Reports one msg 12 the client actually decoded.
 * @param client ActivityClient the handler was called on.
 * @param before Status word before the call.
 * @param after Status word after it.
 */
void report(std::int64_t client, std::uint16_t before, std::uint16_t after) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const std::uint64_t memberKey =
        *reinterpret_cast<const std::uint64_t*>(client + kMembershipHeaderOffset);
    const auto axisOne = *reinterpret_cast<const std::uint32_t*>(client + kSlotAxisOneOffset);
    const auto axisTwo = *reinterpret_cast<const std::uint32_t*>(client + kSlotAxisTwoOffset);
    const auto* label = reinterpret_cast<const char*>(client + kSlotLabelOffset);
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=probe stage=msg12 result=received client=0x%llX "
                                      "slot=%u label=%.*s member=0x%016llX "
                                      "status=0x%04X->0x%04X flag=%u",
                                      static_cast<unsigned long long>(client),
                                      axisOne + 2U * axisTwo,
                                      static_cast<int>(kSlotLabelCapacity),
                                      label,
                                      static_cast<unsigned long long>(memberKey),
                                      static_cast<unsigned>(before),
                                      static_cast<unsigned>(after),
                                      (after & kMembershipFlag) != 0 ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports the four bind inputs the rebind reads, none of which needs a call.
 * A zero established id skips the slot silently, and a null roster container keeps the slot out
 * of the public-first pick, so between them they name which reader can ever see this client.
 * @param client ActivityClient.
 */
void report_bind_inputs(std::int64_t client) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const auto established =
        *reinterpret_cast<const std::uint64_t*>(client + kEstablishedSessionOffset);
    const auto slotRecord = *reinterpret_cast<const std::uint64_t*>(client + kSlotRecordOffset);
    const auto rosterContainer =
        *reinterpret_cast<const std::uint64_t*>(client + kRosterContainerOffset);
    const auto receipt = *reinterpret_cast<const std::uint8_t*>(client + kBindReceiptOffset);
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=probe stage=bind client=0x%llX established=0x%016llX "
                                      "slotrec=0x%llX roster=0x%llX receipt=%u",
                                      static_cast<unsigned long long>(client),
                                      static_cast<unsigned long long>(established),
                                      static_cast<unsigned long long>(slotRecord),
                                      static_cast<unsigned long long>(rosterContainer),
                                      static_cast<unsigned>(receipt));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @param client ActivityClient. @return Entity-slot bits it holds but has not applied. */
[[nodiscard]] std::size_t pending_slots(std::int64_t client) noexcept {
    const auto* mask = reinterpret_cast<const std::uint8_t*>(client + kPendingMaskOffset);
    std::size_t count = 0;
    for (std::size_t index = 0; index < kPendingMaskSize; ++index) {
        count += static_cast<std::size_t>(std::popcount(mask[index]));
    }
    return count;
}

/** Opens or refreshes the sampling window for one client. */
void track(std::int64_t client, std::uint64_t now) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    Tracked* free = nullptr;
    for (Tracked& entry : g_tracked) {
        if (entry.occupied && entry.client == client) {
            entry.expiresAt = now + kSampleWindowMs;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (free == nullptr && !entry.occupied) {
            free = &entry;
        }
    }
    if (free != nullptr) {
        *free = {client, now + kSampleWindowMs, now, true};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Reads the status word, defers to the original, then reads it again. */
char __fastcall receive(std::int64_t client, std::int64_t body, int size) noexcept {
    const auto* original = reinterpret_cast<Receive>(g_handle.original);
    if (original == nullptr || client == 0) {
        return original != nullptr ? original(client, body, size) : 0;
    }
    const auto before = *reinterpret_cast<const std::uint16_t*>(client + kStatusWordOffset);
    const char result = original(client, body, size);
    const auto after = *reinterpret_cast<const std::uint16_t*>(client + kStatusWordOffset);
    report(client, before, after);
    report_bind_inputs(client);
    track(client, GetTickCount64());
    return result;
}

/**
 * Reports what one client did with its grant after the message.
 * @param client ActivityClient.
 */
void sample(std::int64_t client) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const auto status = *reinterpret_cast<const std::uint16_t*>(client + kStatusWordOffset);
    const auto dirty = *reinterpret_cast<const std::uint8_t*>(client + kGrantDirtyOffset);
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=probe stage=grant client=0x%llX status=0x%04X "
        "pending=%zu dirty=%u receipt=%u",
        static_cast<unsigned long long>(client),
        static_cast<unsigned>(status),
        pending_slots(client),
        static_cast<unsigned>(dirty),
        static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(client + kBindReceiptOffset)));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @param reason Short name of the step that failed. @return Always true: a probe never blocks. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=probe stage=msg12 result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

} // namespace

/** Attaches the read-only probe on the client's activity msg 12 handler. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kReceive, "membership_probe_msg12");
    if (target == nullptr) {
        return fail("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&receive)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=probe stage=msg12 result=installed");
    return true;
}

/** Samples every ActivityClient the probe has seen recently. */
void service(std::uint64_t now) noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    // Copied under the lock, sampled outside it: a read walks 1024 bytes and must not hold a lock
    // the detour needs on the client's own thread.
    std::array<std::int64_t, kTrackedCapacity> due{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (Tracked& entry : g_tracked) {
        if (!entry.occupied) {
            continue;
        }
        if (now >= entry.expiresAt) {
            // The window closes before an activity can plausibly tear down, so the pointer is
            // never read once it could be stale.
            entry = {};
            continue;
        }
        if (now >= entry.nextSample) {
            entry.nextSample = now + kSampleIntervalMs;
            due[count] = entry.client;
            ++count;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < count; ++index) {
        sample(due[index]);
    }
}

/** Detaches the probe so a later unload cannot leave a detour into unmapped code. */
bool uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    const bool detached = hooking::detour::uninstall(g_handle);
    g_installed.store(!detached, std::memory_order_release);
    return detached;
}

} // namespace sunrise::client::hooks::membership_probe
