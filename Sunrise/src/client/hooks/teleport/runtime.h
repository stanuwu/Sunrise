#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::teleport {

/** Three floats make one position or velocity vector. */
inline constexpr std::size_t kVectorLanes = 3;
/** The vertical lane. The camera basis is X forward, Z up, so the third lane is up. */
inline constexpr std::size_t kVerticalLane = 2;

/** One world-space vector as the game stores it. */
using Vector = std::array<float, kVectorLanes>;

/** Read-only camera pose published by the existing camera-frame hook. */
struct CameraPose final {
    Vector position{};
    Vector forward{};
    Vector up{};
    float horizontalFov{};
    float aspect{};
};

/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

/** Publishes the camera accessor after its target has been resolved. */
void publish_targets(CameraSingleton singleton) noexcept;

/** Drops those functions and every latched request. */
void clear_targets() noexcept;

/**
 * Attaches the camera and physics hooks that carry the teleport.
 * @return True when all three targets were found and both detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Removes both detours, preserving runtime state when removal fails. */
[[nodiscard]] bool uninstall() noexcept;

/** Publishes the camera pose for the frame and the forward vector for the next physics tick. */
void capture_camera_pose(std::uint32_t playerIndex) noexcept;

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept;

/** Runs the move for a request no physics tick collected, and drives the sync for it. */
void force_pending() noexcept;

/**
 * Finds both key tables from the polled keyboard scan.
 * @return True when the scan and both of its table loads were found.
 */
[[nodiscard]] bool resolve_action_keys() noexcept;

/** Drops both key tables. */
void clear_action_keys() noexcept;

/**
 * Turns one authored binding into the virtual key the scan will read.
 * @param binding Input code taken from an authored binding half.
 * @return The virtual key, or 0 when there is none.
 */
[[nodiscard]] std::uint32_t action_key(std::uint16_t binding) noexcept;

/**
 * Calls the physics sync for one component through the installed trampoline.
 * @param component Physics component to sync.
 */
void invoke_sync(void* component) noexcept;

/**
 * Moves the local player if a request is pending and this component owns them.
 * @param component Physics component about to be synced.
 */
void apply_pending(void* component) noexcept;

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 *
 * Exposed because the physics sync is the only tick that sees every component. A feature acting
 * on the player's own tick needs the same test this module already runs.
 */
[[nodiscard]] bool owns_local_player(void* component) noexcept;

/**
 * Reads the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_position(void* component, Vector& position) noexcept;

/**
 * Reports the physics component the local player was last seen driving.
 * The sync stops for a player at rest, so a frame poll has no other way back to them.
 * @return That component, or null before the player has been seen. Prove it before use.
 */
[[nodiscard]] void* local_player_component() noexcept;

/**
 * Writes the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_velocity(void* component, const Vector& velocity) noexcept;

/**
 * The camera hook is the only site that sees the pose block, so it publishes the vector here.
 * @param forward Receives the camera forward vector published this frame.
 * @return True once the camera hook has published one.
 */
[[nodiscard]] bool camera_forward(Vector& forward) noexcept;

/** Copies the last complete pose published by the camera-frame hook. */
[[nodiscard]] bool camera_pose(CameraPose& pose) noexcept;

} // namespace sunrise::client::hooks::teleport
