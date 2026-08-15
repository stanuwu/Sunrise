/**
 * Finds the three teleport targets and attaches the two detours. All or nothing: finding only some
 * would arm a key that silently does nothing.
 */

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../polled_input/runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

/** Runs per frame on the thread owning the camera and the player, and writes the camera pose. */
constexpr std::string_view kCameraTransformText =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8D 6C 24 E0 48 81 EC 20 01 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 45 10 0F 57 C0 48 63 F9";
/** Compiled pattern bytes of the camera transform signature. */
constexpr auto kCameraTransform =
    signature<signature_length(kCameraTransformText)>(kCameraTransformText);

/** Writes the local player's biped object handle, or the invalid handle value. */
constexpr std::string_view kControlledHandleText =
    "40 53 48 83 EC 20 48 8B D9 C7 01 FF FF FF FF 48 8D 4C 24 30 E8 ? ? ? ? 8B 44 24 30 "
    "83 F8 FF 74 18 25 FF 1F 00 00 0F AF 05";
/** Compiled pattern bytes of the controlled-handle signature. */
constexpr auto kControlledHandlePattern =
    signature<signature_length(kControlledHandleText)>(kControlledHandleText);

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

/** @return The trampoline for a handle slot, or null. */
template <typename T> [[nodiscard]] T original(std::size_t slot) noexcept {
    return reinterpret_cast<T>(g_handles[slot].original);
}

/**
 * Publishes the forward vector and reads the bound key, then defers to the original.
 * @param playerIndex Player whose camera was transformed.
 * @return Whatever the original returns.
 */
std::int64_t __fastcall camera_transform(std::uint32_t playerIndex) noexcept {
    const CameraTransform next = original<CameraTransform>(kCameraSlot);
    const std::int64_t result = next != nullptr ? next(playerIndex) : 0;
    capture_forward(playerIndex);
    poll_request();
    force_pending();
    return result;
}

/**
 * Applies a pending move before the sync runs, so the sync publishes the moved position in the
 * same tick instead of overwriting it.
 * @param component Physics component being synced.
 * @param outFlags The original's second argument, untouched.
 * @return Whatever the original returns.
 */
std::int64_t __fastcall physics_sync(std::byte* component, std::byte* outFlags) noexcept {
    apply_pending(component);
    const PhysicsSync next = original<PhysicsSync>(kPhysicsSlot);
    return next != nullptr ? next(component, outFlags) : 0;
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

} // namespace

/** Attaches the camera and physics hooks that carry the teleport. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const transform = scan_main_image_unique(kCameraTransform, "teleport_camera");
    if (transform == nullptr) {
        return fail("camera");
    }
    std::byte* const handle = scan_main_image_unique(kControlledHandlePattern, "teleport_handle");
    if (handle == nullptr) {
        return fail("handle");
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
    publish_targets(reinterpret_cast<ControlledHandle>(handle), singleton);
    // The injected press needs the game's key tables. Without them the move still lands, it just
    // stays invisible until the player moves, so we log instead of failing.
    if (!resolve_action_keys()) {
        (void)fail("action_keys");
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=teleport stage=install result=ok");
    return true;
}

/** Calls the physics sync for one component through the installed trampoline. */
void invoke_sync(void* component) noexcept {
    const PhysicsSync next = original<PhysicsSync>(kPhysicsSlot);
    if (next == nullptr || component == nullptr) {
        return;
    }
    std::array<std::byte, kSyncFlagsCapacity> flags{};
    (void)next(static_cast<std::byte*>(component), flags.data());
}

/** Detaches both teleport hooks. */
void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    clear_targets();
    clear_action_keys();
    polled_input::release_key();
    (void)hooking::detour::uninstall(g_handles);
    g_handles = {};
}

} // namespace sunrise::client::hooks::teleport
