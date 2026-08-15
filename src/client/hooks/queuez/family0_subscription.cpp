#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "family0/family0_source_seed.h"
#include "internal.h"

namespace sunrise::client::hooks::queuez {
namespace {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** The family sweep that subscribes the account's families. Its argument is the manager. */
constexpr std::string_view kFamilySweepSignatureText =
    "40 53 55 41 56 48 83 EC ? 48 8D 69 30 48 89 7C 24 ? 4C 89 7C 24 ? 4C 8B F1 4C 8D 79 50";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kFamilySweepSignature =
    signature<signature_length(kFamilySweepSignatureText)>(kFamilySweepSignatureText);

/**
 * The family-zero sweep. The prologue alone collides with an unrelated function, so the register
 * saves are matched too. It ends on the call that fetches the source list, whose operand the
 * install decodes, so nothing may be appended.
 */
constexpr std::string_view kFamily0SweepSignatureText =
    "4C 8B DC 55 57 49 8D AB ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 "
    "? ? ? ? 49 89 5B ? 49 89 73 ? 4D 89 63 ? 4D 89 6B ? 4C 8B E9 4D 89 73 ? 4D 89 7B ? "
    "48 89 4C 24 ? E8";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kFamily0SweepSignature =
    signature<signature_length(kFamily0SweepSignatureText)>(kFamily0SweepSignatureText);

/** Signed displacement bytes of a near call, decoded to reach the source-list getter. */
constexpr std::ptrdiff_t kRelativeOperandSize = 4;

using FamilySweep = std::int64_t(__fastcall*)(std::byte*);
using Family0Sweep = void(__fastcall*)(std::byte*);

hooking::detour::Handle g_sweepHandle{};
hooking::detour::Handle g_family0Handle{};
std::atomic<FamilySweep> g_originalSweep{nullptr};
std::atomic<Family0Sweep> g_originalFamily0{nullptr};

/**
 * Seeds the source list, then defers to the original family-zero sweep.
 * @param held Borrowed copy of the list as the sweep last saw it, which the original updates.
 */
__declspec(noinline) void __fastcall family0_sweep(std::byte* held) noexcept {
    family0::seed_source_list();
    const Family0Sweep original = g_originalFamily0.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(held);
    }
}

/** Captures the account key, then defers to the original sweep. */
__declspec(noinline) std::int64_t __fastcall family_sweep(std::byte* manager) noexcept {
    family0::learn_account_key(manager);
    const FamilySweep original = g_originalSweep.load(std::memory_order_acquire);
    return original != nullptr ? original(manager) : 0;
}

/**
 * Finds the family-zero sweep, publishes the source-list getter its signature ends on, and
 * attaches the detour. The getter is decoded from the call operand instead of matched on its own,
 * because its body is a 2-instruction thunk that many siblings share.
 * @return True when the target was found and the detour attached.
 */
[[nodiscard]] bool attach_family0_sweep() noexcept {
    std::byte* const target =
        scan_main_image_unique(kFamily0SweepSignature, "queuez_family0_sweep");
    if (target == nullptr) {
        return false;
    }
    std::byte* const operand = target + kFamily0SweepSignature.size();
    family0::publish_source_list(reinterpret_cast<family0::SourceList>(
        resolve_relative(operand, operand + kRelativeOperandSize)));
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&family0_sweep)};
    return hooking::detour::install(spec, g_family0Handle);
}

/**
 * Finds one signature and attaches its detour.
 * @param signature Bytes that identify the target.
 * @param name Name logged on a miss.
 * @param replacement Detour entry point.
 * @param handle Receives the attached handle.
 * @return True when the target was found and the detour attached.
 */
[[nodiscard]] bool attach(std::span<const patterns::PatternByte> signature,
                          const char* name,
                          void* replacement,
                          hooking::detour::Handle& handle) noexcept {
    std::byte* const target = scan_main_image_unique(signature, name);
    if (target == nullptr) {
        return false;
    }
    const hooking::detour::Spec spec{target, replacement};
    return hooking::detour::install(spec, handle);
}

} // namespace

/**
 * Attaches the family-zero source-list seed. The manager sweep must attach too: it is the only
 * reader of the account key the seed writes, and it runs before the producer on the same thread.
 * @return True when every fix attached.
 */
bool install_family0_subscription() noexcept {
    if (g_sweepHandle.attached) {
        return true;
    }
    const bool swept = attach(kFamilySweepSignature,
                              "queuez_family_sweep",
                              reinterpret_cast<void*>(&family_sweep),
                              g_sweepHandle);
    if (swept) {
        g_originalSweep.store(reinterpret_cast<FamilySweep>(g_sweepHandle.original),
                              std::memory_order_release);
    }
    const bool seeded = attach_family0_sweep();
    if (seeded) {
        g_originalFamily0.store(reinterpret_cast<Family0Sweep>(g_family0Handle.original),
                                std::memory_order_release);
    }
    const bool ok = swept && seeded;
    core::log::write(core::log::Channel::client,
                     ok ? core::log::Level::info : core::log::Level::warn,
                     ok ? "ev=queuez stage=family0_install result=ok"
                        : "ev=queuez stage=family0_install result=fail reason=target");
    return ok;
}

/** Detaches the family-zero source-list seed, in the reverse order of install. */
void uninstall_family0_subscription() noexcept {
    if (g_family0Handle.attached) {
        (void)hooking::detour::uninstall(g_family0Handle);
    }
    if (g_sweepHandle.attached) {
        (void)hooking::detour::uninstall(g_sweepHandle);
    }
    g_originalFamily0.store(nullptr, std::memory_order_release);
    g_originalSweep.store(nullptr, std::memory_order_release);
    family0::reset();
}

/** @return True while either family-zero detour is attached. */
bool family0_subscription_installed() noexcept {
    return g_sweepHandle.attached || g_family0Handle.attached;
}

} // namespace sunrise::client::hooks::queuez
