#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/activity/runtime.h"
#include "bootflow_hook_lifecycle.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The current boot-flow step accessor, `BootFlow_GetStep_NoBubbleArg`* @ `0x7FF742AED510`.
 * Decrypts the manager global and returns `mgr + 912`, or -1 when it is null. Only the call's
 * displacement is wildcarded; the `mgr + 912` field offset makes the pattern unique.
 */
constexpr std::string_view kStepSignatureText =
    "48 83 EC 28 E8 ? ? ? ? 48 85 C0 74 0B 8B 80 90 03 00 00 48 83 C4 28 C3 83 C8 FF 48 83 C4 28 "
    "C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kStepSignature = signature<signature_length(kStepSignatureText)>(kStepSignatureText);

/** First step that loads the map with no player in it yet. */
constexpr std::int32_t kActivityLoadFirst = 33;
/** `setup:orbit`, the only state from which Izanami may request activity creation. */
constexpr std::int32_t kOrbit = 29;
/** `activity:in_world`. The fade is armed by then, so a spawn now releases it. */
constexpr std::int32_t kInWorld = 38;
/** No step has been published. */
constexpr std::int32_t kNoStep = -1;
/** A published step older than this says nothing: the tick that publishes it has stopped. */
constexpr std::uint64_t kStepStaleMs = 1'000;

using GetStep = std::int64_t(__fastcall*)() noexcept;

std::atomic<GetStep> g_step{nullptr};
/** Last step the frame poll read, for readers that are not on the game thread. */
std::atomic_int32_t g_publishedStep{kNoStep};
/** Tick that step was read on. A stale value reads as out of world. */
std::atomic_uint64_t g_publishedTick{0};

/** @return True when a requested step is both current and fresh. */
[[nodiscard]] bool is_fresh_step(std::int32_t expected) noexcept {
    if (g_publishedStep.load(std::memory_order_relaxed) != expected) {
        return false;
    }
    const std::uint64_t published = g_publishedTick.load(std::memory_order_acquire);
    return published != 0 && GetTickCount64() - published < kStepStaleMs;
}

/** @return The current step, or the absent one when the accessor is missing. */
[[nodiscard]] std::int32_t read_step() noexcept {
    const GetStep read = g_step.load(std::memory_order_acquire);
    return read == nullptr ? kNoStep : static_cast<std::int32_t>(read() & 0xFFFFFFFF);
}

} // namespace

/** Publishes the client's own boot-flow step. */
void poll_world_step() noexcept {
    g_publishedStep.store(read_step(), std::memory_order_relaxed);
    g_publishedTick.store(GetTickCount64(), std::memory_order_release);
}

/** Reports whether the player is in a loaded destination. */
bool in_world() noexcept {
    return is_fresh_step(kInWorld);
}

/** Reports whether the client is currently parked in orbit. */
bool in_orbit() noexcept {
    return is_fresh_step(kOrbit);
}

/** Maps the client's own boot-flow step onto the world phase. */
void observe_world_step() noexcept {
    // A missing accessor leaves the phase alone. A step of -1 is a real answer: off a destination.
    if (g_step.load(std::memory_order_acquire) == nullptr) {
        return;
    }
    const std::int32_t step = read_step();
    state::activity::WorldPhase phase = state::activity::WorldPhase::idle;
    if (step == kInWorld) {
        phase = state::activity::WorldPhase::arrived;
    } else if (step >= kActivityLoadFirst && step < kInWorld) {
        phase = state::activity::WorldPhase::transitioning;
    } else {
        // Off a destination, so the next load is a fresh arming and logs its own release line.
        rearm_fade_release();
    }
    state::activity::note_world_phase(phase);
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
}

} // namespace sunrise::client::hooks::bootflow
