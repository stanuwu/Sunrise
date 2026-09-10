/**
 * Finds the three teleport targets and attaches the two detours. All or nothing: finding only some
 * would arm a key that silently does nothing.
 */

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../player/controlled_object.h"
#include "../../player/player_position.h"
#include "../bootflow/bootflow_hook_lifecycle.h"
#include "../fly/fly.h"
#include "../polled_input/runtime.h"
#include "../sword_skate/sword_skate.h"
#include "internal.h"
#include "runtime.h"

#pragma intrinsic(_InterlockedIncrement, _InterlockedDecrement, _InterlockedCompareExchange)

namespace sunrise::client::hooks::teleport {
namespace {

/** Runs per frame on the thread owning the camera and the player, and writes the camera pose. */
constexpr std::string_view kCameraTransformText =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 E0 48 81 EC 20 01 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 45 10 0F 57 C0 48 63 F9";
/** Compiled pattern bytes of the camera transform signature. */
constexpr auto kCameraTransform =
    signature<signature_length(kCameraTransformText)>(kCameraTransformText);

/** Runs per tick, writing the object placement from the rigid body. We must run before it. */
constexpr std::string_view kPhysicsSyncText =
    "4C 8B DC 55 53 56 41 54 41 55 49 8D 6B A1 48 81 EC F0 00 00 00 48 8B 05 ? ? ? ? "
    "48 33 C4 48 89 45 C7 44 0F B6 A9 40 02 00 00";
/** Compiled pattern bytes of the physics sync signature. */
constexpr auto kPhysicsSync = signature<signature_length(kPhysicsSyncText)>(kPhysicsSyncText);

/** The call to the camera singleton getter, measured from the camera transform's own base. */
constexpr std::size_t kSingletonCallOffset = 0x72;
/** A near call is one opcode byte and a signed displacement. */
constexpr std::byte kNearCallOpcode{0xE8};
constexpr std::size_t kNearCallOperand = 1;
constexpr std::size_t kNearCallLength = 5;

/** Both detours are installed together, so one slot each. */
constexpr std::size_t kHandleCount = 2;
constexpr std::size_t kCameraSlot = 0;
constexpr std::size_t kPhysicsSlot = 1;

using CameraTransform = std::int64_t(__fastcall*)(std::uint32_t);
using PhysicsSync = std::int64_t(__fastcall*)(std::byte*, std::byte*);

std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::atomic_bool g_installed{false};
std::atomic_bool g_accepting{false};
SRWLOCK g_lifecycleLock{SRWLOCK_INIT};

/** Counts complete camera, physics, and explicit sync calls. */
alignas(sizeof(LONG)) volatile LONG g_activeCalls{};

/** Reads the counter without locking or waiting during protected removal. */
[[nodiscard]] bool calls_idle() noexcept {
    return _InterlockedCompareExchange(&g_activeCalls, 0, 0) == 0;
}

/** @return The trampoline for a handle slot, or null. */
template <typename T> [[nodiscard]] T original(std::size_t slot) noexcept {
    AcquireSRWLockShared(&g_lifecycleLock);
    const auto next = reinterpret_cast<T>(g_handles[slot].original);
    ReleaseSRWLockShared(&g_lifecycleLock);
    return next;
}

/**
 * Publishes the forward vector and reads the bound key, then defers to the original.
 * @param playerIndex Player whose camera was transformed.
 * @return Whatever the original returns.
 */
__declspec(noinline) std::int64_t camera_transform_body(std::uint32_t playerIndex) noexcept {
    const CameraTransform next = original<CameraTransform>(kCameraSlot);
    const std::int64_t result = next != nullptr ? next(playerIndex) : 0;
    if (g_accepting.load(std::memory_order_acquire)) {
        capture_camera_pose(playerIndex);
        poll_request();
        force_pending();
        // Poll movement features here even when the player's physics tick is idle.
        hooks::fly::poll_toggle();
        client::player::position::poll();
        hooks::bootflow::poll_world_step();
        hooks::bootflow::poll_current_slice_set();
    }
    return result;
}

/** Holds an active-call token until the camera callback and its original have returned. */
__declspec(noinline) std::int64_t __fastcall camera_transform(std::uint32_t playerIndex) noexcept {
    (void)_InterlockedIncrement(&g_activeCalls);
    const std::int64_t result = camera_transform_body(playerIndex);
    (void)_InterlockedDecrement(&g_activeCalls);
    return result;
}

/**
 * Applies a pending move before the sync runs, so the sync publishes the moved position in the
 * same tick instead of overwriting it.
 * @param component Physics component being synced.
 * @param outFlags The original's second argument, untouched.
 * @return Whatever the original returns.
 */
__declspec(noinline) std::int64_t physics_sync_body(std::byte* component,
                                                    std::byte* outFlags) noexcept {
    const PhysicsSync next = original<PhysicsSync>(kPhysicsSlot);
    if (g_accepting.load(std::memory_order_acquire)) {
        apply_pending(component);
        // Share the existing physics detour rather than attaching another one.
        hooks::sword_skate::apply(component);
        hooks::fly::apply(component);
        client::player::position::observe(component);
    }
    return next != nullptr ? next(component, outFlags) : 0;
}

/** Holds an active-call token until the physics callback and its original have returned. */
__declspec(noinline) std::int64_t __fastcall physics_sync(std::byte* component,
                                                          std::byte* outFlags) noexcept {
    (void)_InterlockedIncrement(&g_activeCalls);
    const std::int64_t result = physics_sync_body(component, outFlags);
    (void)_InterlockedDecrement(&g_activeCalls);
    return result;
}

/**
 * Scratch space for the sync's output flags. The sync sets one bit at byte 97, so the buffer only
 * has to be big enough for that, and is never read back.
 */
constexpr std::size_t kSyncFlagsCapacity = 256;

/**
 * Decodes the camera singleton getter from the call inside the camera transform.
 * @param transform Base of the camera transform.
 * @return The getter, or null when the expected call is not there.
 */
[[nodiscard]] CameraSingleton singleton_from(std::byte* transform) noexcept {
    std::byte* const site = transform + kSingletonCallOffset;
    if (*site != kNearCallOpcode) {
        return nullptr;
    }
    return reinterpret_cast<CameraSingleton>(
        resolve_relative(site + kNearCallOperand, site + kNearCallLength));
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=teleport stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Uses the installed physics trampoline while the outer call holds a lifetime token. */
__declspec(noinline) void invoke_sync_body(void* component) noexcept {
    const PhysicsSync next = original<PhysicsSync>(kPhysicsSlot);
    if (next == nullptr || component == nullptr || !g_accepting.load(std::memory_order_acquire)) {
        return;
    }
    std::array<std::byte, kSyncFlagsCapacity> flags{};
    (void)next(static_cast<std::byte*>(component), flags.data());
}

/** Installs both detours while original-pointer publication is exclusively locked. */
[[nodiscard]] bool install_locked() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return g_accepting.load(std::memory_order_acquire) || fail("stopping");
    }
    if (!client::player::controlled_object::available()) {
        return fail("dependency");
    }
    std::byte* const transform = scan_main_image_unique(kCameraTransform, "teleport_camera");
    if (transform == nullptr) {
        return fail("camera");
    }
    std::byte* const sync = scan_main_image_unique(kPhysicsSync, "teleport_sync");
    if (sync == nullptr) {
        return fail("sync");
    }
    const CameraSingleton singleton = singleton_from(transform);
    if (singleton == nullptr) {
        return fail("singleton");
    }

    const std::array<hooking::detour::Spec, kHandleCount> specs{
        hooking::detour::Spec{transform, reinterpret_cast<void*>(&camera_transform)},
        hooking::detour::Spec{sync, reinterpret_cast<void*>(&physics_sync)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        return fail("attach");
    }
    publish_targets(singleton);
    // The injected press needs the game's key tables. Without them the move still lands, it just
    // stays invisible until the player moves, so we log instead of failing.
    if (!resolve_action_keys()) {
        (void)fail("action_keys");
    }
    g_installed.store(true, std::memory_order_release);
    g_accepting.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=teleport stage=install result=ok");
    return true;
}

/** Preserves every target and handle unless protected removal succeeds. */
[[nodiscard]] bool uninstall_locked() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&camera_transform)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&physics_sync)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&invoke_sync)},
    };
    const auto result = hooking::detour::uninstall(g_handles, protectedEntries, &calls_idle);
    if (result != hooking::detour::UninstallResult::removed) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         result == hooking::detour::UninstallResult::protectedCodeActive
                             ? "ev=teleport stage=uninstall result=fail reason=active_calls"
                             : "ev=teleport stage=uninstall result=fail reason=detour");
        return false;
    }
    clear_targets();
    clear_action_keys();
    hooks::fly::reset();
    client::player::position::reset();
    polled_input::release_key();
    // A thread still inside a replacement keeps the detours; the cleared targets make them inert.
    bool replacementActive = false;
    if (!hooking::detour::uninstall(g_handles, replacementActive)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         replacementActive
                             ? "ev=teleport stage=uninstall result=fail reason=active"
                             : "ev=teleport stage=uninstall result=fail reason=detach");
        return false;
    }
    g_handles = {};
    g_installed.store(false, std::memory_order_release);
    return true;
}

} // namespace

/** Attaches the camera and physics hooks that carry the teleport. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    const bool installed = install_locked();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return installed;
}

/** Calls the physics sync for one component through the installed trampoline. */
__declspec(noinline) void invoke_sync(void* component) noexcept {
    (void)_InterlockedIncrement(&g_activeCalls);
    invoke_sync_body(component);
    (void)_InterlockedDecrement(&g_activeCalls);
}

/** Detaches both teleport hooks. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_accepting.store(false, std::memory_order_release);
    const bool removed = uninstall_locked();
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return removed;
}

} // namespace sunrise::client::hooks::teleport
