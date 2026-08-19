#include "navigation_backend.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "navigation_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {
namespace navigation_internal {

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] std::uint32_t next_generation(std::uint32_t generation) noexcept {
    return generation == kExhaustedGeneration ? 0 : generation + 1U;
}

[[nodiscard]] bool generation_reusable(std::uint32_t generation) noexcept {
    return generation != 0 && generation != kExhaustedGeneration;
}

/**
 * Validates one supported navigation agent proxy.
 * @param shape Proxy dimensions and kind.
 * @return True when all active dimensions are finite and positive.
 */
[[nodiscard]] bool valid(const Shape& shape) noexcept {
    switch (shape.kind) {
    case ShapeKind::sphere:
        return finite(shape.radius) && shape.radius > 0.0F;
    case ShapeKind::box:
        return finite(shape.halfExtents) && shape.halfExtents.x > 0.0F && shape.halfExtents.y > 0.0F
               && shape.halfExtents.z > 0.0F;
    case ShapeKind::capsule:
        return finite(shape.radius) && finite(shape.halfHeight) && shape.radius > 0.0F
               && shape.halfHeight >= 0.0F;
    }
    return false;
}

/**
 * Gets a rotation-independent radius for scene-bound checks.
 * @param shape Valid agent proxy.
 * @return Radius which encloses every proxy orientation.
 */
[[nodiscard]] float conservative_radius(const Shape& shape) noexcept {
    switch (shape.kind) {
    case ShapeKind::sphere:
        return shape.radius;
    case ShapeKind::box:
        return std::sqrt(shape.halfExtents.x * shape.halfExtents.x
                         + shape.halfExtents.y * shape.halfExtents.y
                         + shape.halfExtents.z * shape.halfExtents.z);
    case ShapeKind::capsule:
        return shape.radius + shape.halfHeight;
    }
    return 0.0F;
}

[[nodiscard]] bool inside(const Aabb& bounds, Vec3 point, float radius) noexcept {
    return point.x - radius >= bounds.minimum.x && point.y - radius >= bounds.minimum.y
           && point.z - radius >= bounds.minimum.z && point.x + radius <= bounds.maximum.x
           && point.y + radius <= bounds.maximum.y && point.z + radius <= bounds.maximum.z;
}

[[nodiscard]] bool same_position(Vec3 left, Vec3 right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

/**
 * Maps physics query failure without weakening its context gate.
 * @param status Physics query status.
 * @return Matching navigation status.
 */
[[nodiscard]] NavigationStatus map_status(QueryStatus status) noexcept {
    switch (status) {
    case QueryStatus::success:
        return NavigationStatus::success;
    case QueryStatus::invalidQuery:
        return NavigationStatus::invalidQuery;
    case QueryStatus::staleScene:
        return NavigationStatus::staleScene;
    case QueryStatus::contextMismatch:
        return NavigationStatus::contextMismatch;
    }
    return NavigationStatus::invalidQuery;
}

} // namespace navigation_internal

using namespace navigation_internal;

/**
 * Binds fixed navigation storage to one physics backend.
 * @param physics Physics source used by every query.
 */
void NavigationBackend::initialize(const PhysicsBackend& physics) noexcept {
    for (SceneSlot& scene : scenes_) {
        const std::uint32_t next = next_generation(scene.generation);
        const std::uint32_t generation = next == 0 ? scene.generation : next;
        scene = {};
        scene.generation = generation;
    }
    physics_ = &physics;
    initialized_ = true;
}

/** Clears fixed storage and removes the physics binding. */
void NavigationBackend::shutdown() noexcept {
    if (initialized_) {
        for (SceneSlot& scene : scenes_) {
            const std::uint32_t next = next_generation(scene.generation);
            const std::uint32_t generation = next == 0 ? scene.generation : next;
            scene = {};
            scene.generation = generation;
        }
    }
    physics_ = nullptr;
    initialized_ = false;
}

/** @return Capabilities of the safe scriptless fallback. */
NavigationCapabilities NavigationBackend::capabilities() const noexcept {
    return {};
}

/**
 * Loads one build- and bubble-matched scriptless scene.
 * @param spec Exact physics-scene binding.
 * @param handle Receives the fixed navigation slot.
 * @return True when one free slot accepts the binding.
 */
bool NavigationBackend::load_scene(const NavigationSceneSpec& spec,
                                   NavigationSceneHandle& handle) noexcept {
    if (!initialized_ || physics_ == nullptr || spec.stableSceneId == 0 || spec.contentBuild == 0) {
        return false;
    }
    SceneState physicsScene{};
    if (!physics_->read_scene(spec.physicsScene, physicsScene)
        || physicsScene.spec.contentBuild != spec.contentBuild
        || physicsScene.spec.bubble != spec.bubble) {
        return false;
    }
    std::size_t freeSlot = scenes_.size();
    for (std::size_t index = 0; index < scenes_.size(); ++index) {
        if (scenes_[index].occupied
            && (scenes_[index].spec.stableSceneId == spec.stableSceneId
                || scenes_[index].spec.physicsScene == spec.physicsScene)) {
            return false;
        }
        if (!scenes_[index].occupied && generation_reusable(scenes_[index].generation)
            && freeSlot == scenes_.size()) {
            freeSlot = index;
        }
    }
    if (freeSlot == scenes_.size()) {
        return false;
    }
    SceneSlot& scene = scenes_[freeSlot];
    scene.spec = spec;
    scene.bounds = physicsScene.spec.bounds;
    scene.occupied = true;
    handle = {static_cast<std::uint16_t>(freeSlot), scene.generation};
    return true;
}

/**
 * Unloads one exact navigation scene.
 * @param handle Generation-checked navigation scene.
 * @return True when the exact scene was live.
 */
bool NavigationBackend::unload_scene(NavigationSceneHandle handle) noexcept {
    if (!initialized_ || handle.slot >= scenes_.size()) {
        return false;
    }
    SceneSlot& scene = scenes_[handle.slot];
    if (!scene.occupied || scene.generation != handle.generation) {
        return false;
    }
    const std::uint32_t next = next_generation(scene.generation);
    const std::uint32_t generation = next == 0 ? scene.generation : next;
    scene = {};
    scene.generation = generation;
    return true;
}

} // namespace sunrise::server::gameplay::physics::backend
