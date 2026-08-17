#pragma once

namespace sunrise::client::hooks::fly {

/**
 * Fastest speed the game is shown while this hook drives the position itself. Contact with
 * geometry damages the player above roughly this, and the real speed is not needed for movement.
 */
inline constexpr float kPublishedSpeedCap = 8.0F;

/** Reads the toggle key once a frame and flips the switch on the press. */
void poll_toggle() noexcept;

/**
 * Writes the player's velocity on the physics sync, which publishes it.
 * @param component Physics component being synced. Tested for player ownership here.
 */
void apply(void* component) noexcept;

/** @return True while fly is on. */
[[nodiscard]] bool enabled() noexcept;

/**
 * Sets the velocity the coming simulation step integrates. Noclip reads it too.
 * @param body Character rigid body. Live only inside the step hook.
 */
void before_step(void* body) noexcept;

/**
 * Puts back the height the step's gravity took.
 * @param body Character rigid body. Live only inside the step hook.
 * @param heldElsewhere True when noclip carries the vertical lane.
 */
void after_step(void* body, bool heldElsewhere) noexcept;

/** Clears the key state. The switch is a stored setting and survives. */
void reset() noexcept;

} // namespace sunrise::client::hooks::fly
