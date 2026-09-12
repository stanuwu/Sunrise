#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../activity/mission_launch.h"
#include "bootflow_hook_lifecycle.h"
#include "internal.h"
#include "spawn/slice_set_sample.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The current boot-flow step accessor, `BootFlow_GetStep_NoBubbleArg`*.
 * Decrypts the manager global and returns `mgr + 912`, or -1 when it is null. Only the call's
 * displacement is wildcarded; the `mgr + 912` field offset makes the pattern unique.
 */
constexpr std::string_view kStepSignatureText =
    "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 0B 8B 80 90 03 00 00 48 83 C4 28 C3 83 C8 FF 48 83 C4 28 "
    "C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kStepSignature = signature<signature_length(kStepSignatureText)>(kStepSignatureText);

/** `activity:in_world`. */
constexpr std::int32_t kInWorld = 38;
/** No step has been published. */
constexpr std::int32_t kNoStep = -1;
/** Distinguishes a missing target from an addressable manager with no current slice set. */
constexpr std::int32_t kSliceSetUnavailable = -2;
/** A published step older than this says nothing: the tick that publishes it has stopped. */
constexpr std::uint64_t kStepStaleMs = 1'000;

using GetStep = std::int64_t(__fastcall*)() noexcept;

std::atomic<GetStep> g_step{nullptr};
/** Last step the frame poll read, for readers that are not on the game thread. */
std::atomic_int32_t g_publishedStep{kNoStep};
/** Tick that step was read on. A stale value reads as out of world. */
std::atomic_uint64_t g_publishedTick{0};
/** Last current slice-set sample, or one of the negative sentinels above. */
std::atomic_int32_t g_publishedSliceSet{kSliceSetUnavailable};
std::atomic_uint64_t g_publishedSliceSetTick{0};

/** @return The current step, or the absent one when the accessor is missing. */
[[nodiscard]] std::int32_t read_step() noexcept {
    const GetStep read = g_step.load(std::memory_order_acquire);
    return read == nullptr ? kNoStep : static_cast<std::int32_t>(read() & 0xFFFFFFFF);
}

} // namespace

/** Publishes the client's own boot-flow step. */
void poll_world_step() noexcept {
    const std::int32_t step = read_step();
    g_publishedStep.store(step, std::memory_order_relaxed);
    g_publishedTick.store(GetTickCount64(), std::memory_order_release);
    client::activity::mission_launch::poll(step);
}

/** Publishes the client's current local slice-set index. */
void poll_current_slice_set() noexcept {
    const std::int32_t index = spawn::sample_current_slice_set();
    g_publishedSliceSet.store(index, std::memory_order_relaxed);
    g_publishedSliceSetTick.store(GetTickCount64(), std::memory_order_release);
}

/** Reads the last fresh local slice-set sample. */
CurrentSliceSet current_slice_set() noexcept {
    CurrentSliceSet value{};
    const std::uint64_t published = g_publishedSliceSetTick.load(std::memory_order_acquire);
    if (published == 0 || GetTickCount64() - published >= kStepStaleMs) {
        return value;
    }
    const std::int32_t index = g_publishedSliceSet.load(std::memory_order_relaxed);
    value.available = index != kSliceSetUnavailable;
    value.present = index >= 0;
    value.index = value.present ? index : -1;
    return value;
}

/** Reports whether the player is in a loaded destination. */
bool in_world() noexcept {
    if (g_publishedStep.load(std::memory_order_relaxed) != kInWorld) {
        return false;
    }
    const std::uint64_t published = g_publishedTick.load(std::memory_order_acquire);
    return published != 0 && GetTickCount64() - published < kStepStaleMs;
}

/** Finds the boot-flow step accessor. */
bool install_world_step() noexcept {
    std::byte* const target = scan_main_image_unique(kStepSignature, "bootflow_current_step");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=world_step result=fail reason=target");
        return false;
    }
    g_step.store(reinterpret_cast<GetStep>(target), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=world_step result=ok");
    return true;
}

/** Clears the boot-flow step accessor it found. */
void uninstall_world_step() noexcept {
    g_step.store(nullptr, std::memory_order_release);
    g_publishedSliceSet.store(kSliceSetUnavailable, std::memory_order_relaxed);
    g_publishedSliceSetTick.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
