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

/** Writes the local player's controlled-object handle, or the invalid sentinel. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

/**
 * Publishes the two functions the hooks call.
 * @param controlled Writes the local player's object handle.
 * @param singleton Returns the camera pose block array.
 */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;

/** Drops those functions and every latched request. */
void clear_targets() noexcept;

/** @return True when an object record belongs to the locally controlled player. */
[[nodiscard]] bool is_controlled_object(const void* object) noexcept;

/** Reads the local player's current world position. */
[[nodiscard]] bool current_position(Vector& output) noexcept;

/** Reads the local player's controlled-object handle. */
[[nodiscard]] bool current_controlled_handle(std::uint32_t& output) noexcept;

/** Reads the most recently published camera position and forward vector. */
[[nodiscard]] bool current_camera_pose(Vector& position, Vector& forward) noexcept;

/**
 * Attaches the camera and physics hooks that carry the teleport.
 * @return True when all three targets were found and both detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both teleport hooks. */
void uninstall() noexcept;

/**
 * Publishes the camera forward vector for the physics tick that follows.
 * @param playerIndex Player the camera pose block belongs to.
 */
void capture_forward(std::uint32_t playerIndex) noexcept;

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
 * Exposed because the physics sync is the only tick that sees every component, and a feature that
 * has to act on the player's own tick needs the same test this module already performs.
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
 * Writes the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_position(void* component, const Vector& position) noexcept;

/**
 * Moves the local player's rigid body to an absolute world position and publishes it
 * immediately.
 * @return True when a live local-player component accepted the move.
 */
[[nodiscard]] bool move_local_player(const Vector& position) noexcept;

/**
 * Reads the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_velocity(void* component, Vector& velocity) noexcept;

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

} // namespace sunrise::client::hooks::teleport
