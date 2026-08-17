#pragma once

#include <array>

namespace sunrise::client::hooks::noclip {

/** Three lanes of a Havok vector. A write leaves the stored fourth lane alone. */
using Vector = std::array<float, 3>;

/**
 * Resolves the Havok targets and attaches the independent simulation-step detour.
 * @return True when both signatures resolve and the detour is attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the simulation-step detour and clears runtime state. */
void uninstall() noexcept;

/**
 * Reads a live rigid body's world position.
 * @param body Body from the simulation hook. Valid only inside that call.
 * @param position Receives the three lanes.
 */
void read_body_position(void* body, Vector& position) noexcept;

/** Writes a live rigid body's world position. @see read_body_position */
void write_body_position(void* body, const Vector& position) noexcept;

/** Reads a live rigid body's linear velocity. @see read_body_position */
void read_body_velocity(void* body, Vector& velocity) noexcept;

/** Writes a live rigid body's linear velocity. @see read_body_position */
void write_body_velocity(void* body, const Vector& velocity) noexcept;

} // namespace sunrise::client::hooks::noclip
