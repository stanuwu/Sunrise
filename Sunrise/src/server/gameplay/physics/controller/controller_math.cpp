#include <algorithm>
#include <cmath>
#include <cstdint>

#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller::internal {
namespace {

/** Values below this bound cannot yield stable normalized vectors. */
constexpr float kMinimumMagnitude = 1.0E-6F;

[[nodiscard]] double magnitude(backend::Vec3 value) noexcept {
    return std::hypot(
        static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z));
}

[[nodiscard]] backend::Quaternion normalize(backend::Quaternion value) noexcept {
    const double valueMagnitude =
        std::hypot(std::hypot(static_cast<double>(value.x), static_cast<double>(value.y)),
                   std::hypot(static_cast<double>(value.z), static_cast<double>(value.w)));
    if (!std::isfinite(valueMagnitude) || valueMagnitude <= kMinimumMagnitude) {
        return {};
    }
    const float inverse = static_cast<float>(1.0 / valueMagnitude);
    return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
}

[[nodiscard]] backend::Quaternion desired_facing(backend::Vec3 direction) noexcept {
    const float yaw = std::atan2(direction.x, direction.z);
    const float halfYaw = yaw * 0.5F;
    return {0.0F, std::sin(halfYaw), 0.0F, std::cos(halfYaw)};
}

} // namespace

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool finite(backend::Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(world::Vector3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

backend::Vec3 to_backend(world::Vector3 value) noexcept {
    return {value.x, value.y, value.z};
}

world::Vector3 to_world(backend::Vec3 value) noexcept {
    return {value.x, value.y, value.z};
}

bool actor_equal(world::ActorKey left, world::ActorKey right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool actor_valid(world::ActorKey actor) noexcept {
    return actor.id != 0 && actor.generation != 0;
}

bool blueprint_equal(world::BlueprintRef left, world::BlueprintRef right) noexcept {
    return left.id == right.id && left.version == right.version;
}

bool blueprint_valid(world::BlueprintRef blueprint) noexcept {
    return blueprint.id != 0 && blueprint.version != 0;
}

/** @param profile Versioned mechanics limits. @return True when every limit is safe. */
bool valid_profile(const ControllerProfile& profile) noexcept {
    if (!blueprint_valid(profile.controller) || !blueprint_valid(profile.navigationProfile)
        || !finite(profile.maxSpeed) || profile.maxSpeed <= 0.0F || !finite(profile.maxAcceleration)
        || profile.maxAcceleration <= 0.0F || !finite(profile.maxTurnRadiansPerSecond)
        || profile.maxTurnRadiansPerSecond < 0.0F || !finite(profile.arrivalRadius)
        || profile.arrivalRadius <= 0.0F || !finite(profile.waypointRadius)
        || profile.waypointRadius < profile.arrivalRadius || !finite(profile.progressEpsilon)
        || profile.progressEpsilon <= 0.0F || profile.stuckTickLimit == 0) {
        return false;
    }
    return profile.facing == FacingMode::none || profile.maxTurnRadiansPerSecond > 0.0F;
}

float length(backend::Vec3 value) noexcept {
    return static_cast<float>(magnitude(value));
}

float distance(backend::Vec3 left, backend::Vec3 right) noexcept {
    return length(subtract(left, right));
}

backend::Vec3 subtract(backend::Vec3 left, backend::Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

backend::Vec3 add(backend::Vec3 left, backend::Vec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

backend::Vec3 multiply(backend::Vec3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

/**
 * Clamps desired speed and per-tick velocity change.
 * @param current Last committed controller velocity.
 * @param desired Velocity toward the active waypoint.
 * @param maxDelta Maximum velocity change this tick.
 * @param maxSpeed Maximum absolute speed.
 * @return Finite bounded velocity for the next target.
 */
backend::Vec3 bounded_velocity(backend::Vec3 current,
                               backend::Vec3 desired,
                               float maxDelta,
                               float maxSpeed) noexcept {
    const double desiredLength = magnitude(desired);
    if (desiredLength > maxSpeed) {
        desired = multiply(desired, static_cast<float>(maxSpeed / desiredLength));
    }
    backend::Vec3 delta = subtract(desired, current);
    const double deltaLength = magnitude(delta);
    if (deltaLength > maxDelta) {
        delta = multiply(delta, static_cast<float>(maxDelta / deltaLength));
    }
    backend::Vec3 result = add(current, delta);
    const double resultLength = magnitude(result);
    if (resultLength > maxSpeed) {
        result = multiply(result, static_cast<float>(maxSpeed / resultLength));
    }
    return result;
}

/**
 * Rotates from the current pose toward a canonical horizontal direction.
 * @param current Current canonical body rotation.
 * @param direction Desired world-space look direction.
 * @param maxRadians Maximum angular change this tick.
 * @return Normalized rotation within the turn bound.
 */
backend::Quaternion
bounded_facing(backend::Quaternion current, backend::Vec3 direction, float maxRadians) noexcept {
    direction.y = 0.0F;
    if (!finite(direction) || length(direction) <= kMinimumMagnitude || maxRadians <= 0.0F) {
        return normalize(current);
    }
    current = normalize(current);
    backend::Quaternion desired = desired_facing(direction);
    float dot = current.x * desired.x + current.y * desired.y + current.z * desired.z
                + current.w * desired.w;
    if (dot < 0.0F) {
        desired = {-desired.x, -desired.y, -desired.z, -desired.w};
        dot = -dot;
    }
    dot = std::clamp(dot, 0.0F, 1.0F);
    const float halfAngle = std::acos(dot);
    if (halfAngle <= kMinimumMagnitude || maxRadians >= halfAngle * 2.0F) {
        return desired;
    }
    const float fraction = (maxRadians * 0.5F) / halfAngle;
    const float denominator = std::sin(halfAngle);
    if (std::abs(denominator) <= kMinimumMagnitude) {
        return normalize(current);
    }
    const float currentWeight = std::sin((1.0F - fraction) * halfAngle) / denominator;
    const float desiredWeight = std::sin(fraction * halfAngle) / denominator;
    return normalize({current.x * currentWeight + desired.x * desiredWeight,
                      current.y * currentWeight + desired.y * desiredWeight,
                      current.z * currentWeight + desired.z * desiredWeight,
                      current.w * currentWeight + desired.w * desiredWeight});
}

/**
 * Resolves one profile-selected look direction without choosing a target.
 * @param mode Versioned facing mechanic.
 * @param commandedVelocity Current bounded steering velocity.
 * @param facingPosition Policy-supplied fixed look point.
 * @param position Current canonical actor position.
 * @param targetActor Exact policy-supplied actor, when required.
 * @return Canonical direction or zero when facing is disabled.
 */
backend::Vec3 facing_direction(FacingMode mode,
                               backend::Vec3 commandedVelocity,
                               world::Vector3 facingPosition,
                               backend::Vec3 position,
                               const world::ActorSnapshot* targetActor) noexcept {
    switch (mode) {
    case FacingMode::none:
        return {};
    case FacingMode::velocity:
        return commandedVelocity;
    case FacingMode::position:
        return subtract(to_backend(facingPosition), position);
    case FacingMode::actor:
        return targetActor == nullptr
                   ? backend::Vec3{}
                   : subtract(to_backend(targetActor->transform.position), position);
    }
    return {};
}

const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                       world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (actor_equal(candidate.key, actor)) {
            return &candidate;
        }
    }
    return nullptr;
}

std::uint32_t next_generation(std::uint32_t generation) noexcept {
    return generation == backend::kExhaustedGeneration ? 0 : generation + 1U;
}

bool generation_reusable(std::uint32_t generation) noexcept {
    return generation != 0 && generation != backend::kExhaustedGeneration;
}

} // namespace sunrise::server::gameplay::physics::controller::internal
