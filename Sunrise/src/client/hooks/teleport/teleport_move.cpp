/**
 * The teleport itself. The camera hook publishes a forward vector and reads the bound key once a
 * frame. The physics hook applies the move before the sync it runs ahead of. Physics owns the
 * position, so writing the object placement would move the camera alone.
 */

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../../player/controlled_object.h"
#include "../polled_input/runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

/**
 * Frames a press stays pending. Orbit and loading screens tick the camera but never the player's
 * physics, so a request with no limit is used up later and reads as a queued teleport.
 */
constexpr std::uint32_t kRequestLifetimeFrames = 3;
/** Frames an ordinary physics tick gets to collect a request before the forced path takes it. */
constexpr std::uint32_t kForceAfterFrames = 1;

/**
 * Frames the injected press is held. It has to survive at least one scan and one integration
 * step, or the move it exists to publish is never read.
 */
constexpr std::uint32_t kPressFrames = 2;
/** Authored action driven to wake the body. Forward is the gentlest one that moves it. */
constexpr std::uint16_t kForwardAction =
    static_cast<std::uint16_t>(state::account::settings::bindings::Action::moveForward);

std::atomic_bool g_requested{false};
std::atomic_bool g_forwardValid{false};
std::atomic_bool g_keyDown{false};
std::atomic_uint32_t g_requestAge{0};
/** Set while the feature is usable, so the per-tick path costs one atomic read when it is not. */
std::atomic_bool g_active{false};
SRWLOCK g_cameraPoseLock{SRWLOCK_INIT};
CameraPose g_cameraPose{};
bool g_cameraPoseValid{};

/**
 * The player's physics component, kept from the last tick that carried it. At rest the sync stops
 * being called for the player at all, so the pointer is the only way back to them.
 */
std::atomic<std::byte*> g_playerComponent{nullptr};
/** Frames left before the injected press is released. */
std::atomic_uint32_t g_pressFrames{0};

CameraSingleton g_cameraSingleton{};

/** Camera forward vector for the next physics tick. Every access holds g_cameraPoseLock. */
std::array<float, kVectorLanes> g_forward{};

/** Withdraws the pose when the camera block is not readable for this frame. */
void invalidate_camera_pose() noexcept {
    AcquireSRWLockExclusive(&g_cameraPoseLock);
    g_cameraPose = {};
    g_cameraPoseValid = false;
    ReleaseSRWLockExclusive(&g_cameraPoseLock);
}

/** @param forward Receives the published camera forward vector, copied under the pose lock. */
void copy_forward(Vector& forward) noexcept {
    AcquireSRWLockShared(&g_cameraPoseLock);
    forward = g_forward;
    ReleaseSRWLockShared(&g_cameraPoseLock);
}

/**
 * Reads one value out of game memory without faulting on a torn pointer.
 * @param address Source address.
 * @param value Receives the value.
 * @return True when Windows copied the whole value.
 */
template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/**
 * Writes one vector into game memory. The call applies page protection itself.
 * @param address Destination address.
 * @param value Three lanes to store.
 * @return True when Windows copied the whole vector.
 */
[[nodiscard]] bool write_vector(std::byte* address,
                                const std::array<float, kVectorLanes>& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T written = 0;
    const SIZE_T size = sizeof(float) * kVectorLanes;
    return WriteProcessMemory(GetCurrentProcess(), address, value.data(), size, &written) != FALSE
           && written == size;
}

/**
 * Finds the rigid body a physics component drives.
 * @param component Physics component.
 * @return The body, or null when the chain breaks.
 */
[[nodiscard]] std::byte* body_of(std::byte* component) noexcept {
    std::byte* array = nullptr;
    std::int32_t index = 0;
    if (!read_at(component + kPhysicsComponentBodyArray, array)
        || !read_at(component + kPhysicsComponentBodyIndex, index) || array == nullptr
        || index < 0) {
        return nullptr;
    }
    std::byte* body = nullptr;
    const std::size_t offset = kBodyEntryStride * static_cast<std::size_t>(index) + kBodyPointer;
    return read_at(array + offset, body) ? body : nullptr;
}

/**
 * Ages a pending request and drops it once nothing has taken it. A press is meant for the moment
 * it is made, so one that finds no player physics tick is dropped, not held for the next one.
 */
void expire_request() noexcept {
    if (!g_requested.load(std::memory_order_acquire)) {
        return;
    }
    if (g_requestAge.fetch_add(1, std::memory_order_relaxed) + 1 >= kRequestLifetimeFrames) {
        g_requested.store(false, std::memory_order_release);
    }
}

/**
 * Runs the whole move for a component already proved to be the player's.
 * @param component Physics component driving the player.
 * @return True when the body was found and its position was written.
 */
[[nodiscard]] bool perform_move(std::byte* component) noexcept;

/** @param reason Key naming the step that stopped the move. */
void report_skip(const char* reason) noexcept;

/**
 * Starts the injected press that wakes the body.
 * Nothing reads the new body position until something integrates it. So the move drives the
 * player's own forward action, instead of writing what that action would have produced.
 */
void begin_press() noexcept {
    const state::AccountState account = state::account_snapshot();
    const auto& binding = account.settings.keyBindings.values[kForwardAction];
    if (!binding.primary.has_value()) {
        return;
    }
    const std::uint32_t virtualKey = action_key(*binding.primary);
    if (virtualKey == 0) {
        report_skip("no_key");
        return;
    }
    hooks::polled_input::hold_key(virtualKey);
    g_pressFrames.store(kPressFrames, std::memory_order_release);
}

/** Releases the injected press once it has been scanned. */
void end_press() noexcept {
    if (g_pressFrames.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (g_pressFrames.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        hooks::polled_input::release_key();
    }
}

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 */
[[nodiscard]] bool owns_player(std::byte* component) noexcept {
    std::uint32_t controlled = kInvalidHandle;
    if (!client::player::controlled_object::current_handle(controlled)) {
        return false;
    }
    std::uint16_t owner = 0;
    return read_at(component + kPhysicsComponentObjectHandle, owner)
           && (controlled & kHandleIndexMask)
                  == (static_cast<std::uint32_t>(owner) & kHandleIndexMask);
}

/** @param reason Key naming the step that stopped the move. */
void report_skip(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=teleport stage=move result=skip reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Writes one vertical velocity, leaving run momentum on the other two lanes.
 * @param body Rigid body to write.
 * @param value Vertical velocity to store.
 */
void set_vertical_velocity(std::byte* body, float value) noexcept {
    std::array<float, kVectorLanes> velocity{};
    if (!read_at(body + kBodyVelocityX, velocity)) {
        return;
    }
    velocity[kVerticalLane] = value;
    (void)write_vector(body + kBodyVelocityX, velocity);
}

/**
 * Adds one world delta to a stored position.
 * @param address Vector to move.
 * @param delta World units per lane.
 * @param before Receives the value read.
 * @param after Receives the value written.
 * @return True when the new value was stored.
 */
[[nodiscard]] bool offset_vector(std::byte* address,
                                 const std::array<float, kVectorLanes>& delta,
                                 std::array<float, kVectorLanes>& before,
                                 std::array<float, kVectorLanes>& after) noexcept {
    if (!read_at(address, before)) {
        return false;
    }
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        after[lane] = before[lane] + delta[lane];
    }
    return write_vector(address, after);
}

/**
 * Adds the configured distance along the published forward vector.
 *
 * Only the rigid body is written. The physics component's own vector is composed against the body
 * orientation rather than added to it, so a world delta applied there corrupts the transform.
 *
 * @param body Rigid body being moved.
 * @param distance World units to travel.
 * @return True when the new position was stored.
 */
[[nodiscard]] bool move_body(std::byte* body, float distance) noexcept {
    Vector forward{};
    copy_forward(forward);
    std::array<float, kVectorLanes> delta{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        delta[lane] = forward[lane] * distance;
    }
    std::array<float, kVectorLanes> position{};
    std::array<float, kVectorLanes> moved{};
    if (!offset_vector(body + kBodyPositionX, delta, position, moved)) {
        report_skip("body");
        return false;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=move result=ok dist=%.1f "
                                      "from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f",
                                      static_cast<double>(distance),
                                      static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2]),
                                      static_cast<double>(moved[0]),
                                      static_cast<double>(moved[1]),
                                      static_cast<double>(moved[2]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Runs the whole move for a component already proved to be the player's.
 * @param component Physics component driving the player.
 * @return True when the body was found and its position was written.
 */
[[nodiscard]] bool perform_move(std::byte* component) noexcept {
    std::byte* const body = body_of(component);
    if (body == nullptr) {
        report_skip("no_body");
        return false;
    }
    set_vertical_velocity(body, 0.0F);
    if (!move_body(body, client::movement::get().distance)) {
        return false;
    }
    begin_press();
    return true;
}

} // namespace

/** Publishes the camera accessor used by the teleport hook. */
void publish_targets(CameraSingleton singleton) noexcept {
    g_cameraSingleton = singleton;
}

/** Drops the camera accessor and every latched teleport request. */
void clear_targets() noexcept {
    g_cameraSingleton = nullptr;
    g_requested.store(false, std::memory_order_release);
    g_forwardValid.store(false, std::memory_order_release);
    g_keyDown.store(false, std::memory_order_relaxed);
    g_requestAge.store(0, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    g_playerComponent.store(nullptr, std::memory_order_relaxed);
    invalidate_camera_pose();
}

/** Publishes the frame's complete camera pose and its forward vector. */
void capture_camera_pose(std::uint32_t playerIndex) noexcept {
    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) {
        invalidate_camera_pose();
        return;
    }
    std::byte* const camera = g_cameraSingleton();
    if (camera == nullptr) {
        invalidate_camera_pose();
        return;
    }
    const std::size_t playerOffset = kCameraBlockStride * playerIndex;
    CameraPose pose{};
    if (!read_at(camera + playerOffset + kCameraPositionX, pose.position)
        || !read_at(camera + playerOffset + kCameraForwardX, pose.forward)
        || !read_at(camera + playerOffset + kCameraUpX, pose.up)
        || !read_at(camera + playerOffset + kCameraHorizontalFov, pose.horizontalFov)
        || !read_at(camera + playerOffset + kCameraAspect, pose.aspect)) {
        invalidate_camera_pose();
        return;
    }
    AcquireSRWLockExclusive(&g_cameraPoseLock);
    g_cameraPose = pose;
    g_cameraPoseValid = true;
    g_forward = pose.forward;
    ReleaseSRWLockExclusive(&g_cameraPoseLock);
    g_forwardValid.store(true, std::memory_order_release);
}

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept {
    end_press();
    expire_request();
    const client::movement::Settings settings = client::movement::get();
    const bool usable = settings.enabled && settings.virtualKey != client::movement::kNoKey;
    g_active.store(usable, std::memory_order_relaxed);
    if (!usable) {
        g_keyDown.store(false, std::memory_order_relaxed);
        return;
    }
    // An open interface owns the keyboard, so the key that binds the teleport must not fire it.
    if (core::ui::runtime::snapshot().visible) {
        g_keyDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down = client::input::game_focused()
                      && (GetAsyncKeyState(static_cast<int>(settings.virtualKey)) & 0x8000) != 0;
    if (down && !g_keyDown.exchange(down, std::memory_order_relaxed)) {
        g_requestAge.store(0, std::memory_order_relaxed);
        g_requested.store(true, std::memory_order_release);
        return;
    }
    g_keyDown.store(down, std::memory_order_relaxed);
}

/** Moves the local player if a request is pending and this component owns them. */
void apply_pending(void* component) noexcept {
    if (!g_active.load(std::memory_order_relaxed) || component == nullptr
        || !client::player::controlled_object::available()) {
        return;
    }
    const bool requested = g_requested.load(std::memory_order_acquire);
    // The ownership test runs per component, so it is paid only while a request is open or until
    // the player's component is known. Once it is known, an ordinary tick costs two atomic reads.
    if (!requested && g_playerComponent.load(std::memory_order_relaxed) != nullptr) {
        return;
    }
    if (!owns_player(static_cast<std::byte*>(component))) {
        return;
    }
    std::byte* const physics = static_cast<std::byte*>(component);
    g_playerComponent.store(physics, std::memory_order_relaxed);
    if (!requested || !g_forwardValid.load(std::memory_order_acquire)) {
        return;
    }
    g_requested.store(false, std::memory_order_release);
    (void)perform_move(physics);
}

/** Runs the move for a request no physics tick collected. */
void force_pending() noexcept {
    if (!g_requested.load(std::memory_order_acquire)
        || !g_forwardValid.load(std::memory_order_acquire)
        || g_requestAge.load(std::memory_order_relaxed) < kForceAfterFrames) {
        return;
    }
    std::byte* const physics = g_playerComponent.load(std::memory_order_relaxed);
    // The cached pointer outlives a destination change, so it is proved again before use.
    if (physics == nullptr || !client::player::controlled_object::available() || !owns_player(physics)) {
        return;
    }
    g_requested.store(false, std::memory_order_release);
    if (!perform_move(physics)) {
        return;
    }
    invoke_sync(physics);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=teleport stage=force result=ok");
}

/** Reports the physics component the local player was last seen driving. */
void* local_player_component() noexcept {
    return g_playerComponent.load(std::memory_order_relaxed);
}

/** @param component Candidate physics component. @return True when the local player drives it. */
bool owns_local_player(void* component) noexcept {
    return component != nullptr && owns_player(static_cast<std::byte*>(component));
}

/** Reads the world position of the body a physics component drives. */
bool read_position(void* component, Vector& position) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && read_at(body + kBodyPositionX, position);
}

/** Writes the linear velocity of the body a physics component drives. */
bool write_velocity(void* component, const Vector& velocity) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && write_vector(body + kBodyVelocityX, velocity);
}

/** Reports the camera forward vector published this frame. */
bool camera_forward(Vector& forward) noexcept {
    if (!g_forwardValid.load(std::memory_order_acquire)) {
        return false;
    }
    copy_forward(forward);
    return true;
}

/** Copies the last complete pose published by the camera-frame hook. */
bool camera_pose(CameraPose& pose) noexcept {
    AcquireSRWLockShared(&g_cameraPoseLock);
    const bool valid = g_cameraPoseValid;
    pose = valid ? g_cameraPose : CameraPose{};
    ReleaseSRWLockShared(&g_cameraPoseLock);
    return valid;
}

} // namespace sunrise::client::hooks::teleport
