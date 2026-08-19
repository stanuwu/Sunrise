#pragma once

#include <span>

#include "actor_controller_service.h"

namespace sunrise::server::gameplay::physics::controller::internal {

[[nodiscard]] bool finite(float value) noexcept;
[[nodiscard]] bool finite(backend::Vec3 value) noexcept;
[[nodiscard]] bool finite(world::Vector3 value) noexcept;
[[nodiscard]] backend::Vec3 to_backend(world::Vector3 value) noexcept;
[[nodiscard]] world::Vector3 to_world(backend::Vec3 value) noexcept;
[[nodiscard]] bool actor_equal(world::ActorKey left, world::ActorKey right) noexcept;
[[nodiscard]] bool actor_valid(world::ActorKey actor) noexcept;
[[nodiscard]] bool blueprint_equal(world::BlueprintRef left, world::BlueprintRef right) noexcept;
[[nodiscard]] bool blueprint_valid(world::BlueprintRef blueprint) noexcept;
[[nodiscard]] bool valid_profile(const ControllerProfile& profile) noexcept;
[[nodiscard]] float length(backend::Vec3 value) noexcept;
[[nodiscard]] float distance(backend::Vec3 left, backend::Vec3 right) noexcept;
[[nodiscard]] backend::Vec3 subtract(backend::Vec3 left, backend::Vec3 right) noexcept;
[[nodiscard]] backend::Vec3 add(backend::Vec3 left, backend::Vec3 right) noexcept;
[[nodiscard]] backend::Vec3 multiply(backend::Vec3 value, float scalar) noexcept;
[[nodiscard]] backend::Vec3 bounded_velocity(backend::Vec3 current,
                                             backend::Vec3 desired,
                                             float maxDelta,
                                             float maxSpeed) noexcept;
[[nodiscard]] backend::Quaternion
bounded_facing(backend::Quaternion current, backend::Vec3 direction, float maxRadians) noexcept;
[[nodiscard]] backend::Vec3 facing_direction(FacingMode mode,
                                             backend::Vec3 commandedVelocity,
                                             world::Vector3 facingPosition,
                                             backend::Vec3 position,
                                             const world::ActorSnapshot* targetActor) noexcept;
[[nodiscard]] const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                                     world::ActorKey actor) noexcept;
[[nodiscard]] std::uint32_t next_generation(std::uint32_t generation) noexcept;
[[nodiscard]] bool generation_reusable(std::uint32_t generation) noexcept;

} // namespace sunrise::server::gameplay::physics::controller::internal
