#include "world_types.h"

#include <cmath>

namespace sunrise::server::gameplay::physics::world {

bool equal_world(const WorldIdentity& left, const WorldIdentity& right) noexcept {
    return left.activitySessionId == right.activitySessionId
           && left.worldGeneration == right.worldGeneration
           && left.ownerGeneration == right.ownerGeneration;
}

bool equal_actor(const ActorKey& left, const ActorKey& right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool valid_world(const WorldIdentity& identity) noexcept {
    return identity.activitySessionId != 0 && identity.worldGeneration != 0
           && identity.ownerGeneration != 0;
}

bool valid_actor(const ActorKey& actor) noexcept {
    return actor.id != 0 && actor.generation != 0;
}

bool valid_blueprint(const BlueprintRef& blueprint) noexcept {
    return blueprint.id != 0 && blueprint.version != 0;
}

/** @return True when every transform component and its rotation length are finite. */
bool finite_transform(const Transform& transform) noexcept {
    const Quaternion& rotation = transform.rotation;
    const bool finite = std::isfinite(transform.position.x) && std::isfinite(transform.position.y)
                        && std::isfinite(transform.position.z) && std::isfinite(rotation.x)
                        && std::isfinite(rotation.y) && std::isfinite(rotation.z)
                        && std::isfinite(rotation.w);
    const double x = rotation.x;
    const double y = rotation.y;
    const double z = rotation.z;
    const double w = rotation.w;
    const double lengthSquared = x * x + y * y + z * z + w * w;
    return finite && std::isfinite(lengthSquared) && lengthSquared > 0.0F;
}

/** Canonicalizes signed zero and one quaternion hemisphere after robust normalization. */
bool canonicalize_transform(Transform& transform) noexcept {
    if (!finite_transform(transform)) {
        return false;
    }
    Quaternion& rotation = transform.rotation;
    const double x = rotation.x;
    const double y = rotation.y;
    const double z = rotation.z;
    const double w = rotation.w;
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(length) || length == 0.0) {
        return false;
    }
    rotation.x = static_cast<float>(x / length);
    rotation.y = static_cast<float>(y / length);
    rotation.z = static_cast<float>(z / length);
    rotation.w = static_cast<float>(w / length);
    const bool negative =
        rotation.w < 0.0F
        || (rotation.w == 0.0F
            && (rotation.z < 0.0F
                || (rotation.z == 0.0F
                    && (rotation.y < 0.0F || (rotation.y == 0.0F && rotation.x < 0.0F)))));
    if (negative) {
        rotation.x = -rotation.x;
        rotation.y = -rotation.y;
        rotation.z = -rotation.z;
        rotation.w = -rotation.w;
    }
    float* values[] = {&transform.position.x,
                       &transform.position.y,
                       &transform.position.z,
                       &rotation.x,
                       &rotation.y,
                       &rotation.z,
                       &rotation.w};
    for (float* value : values) {
        if (*value == 0.0F) {
            *value = 0.0F;
        }
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
