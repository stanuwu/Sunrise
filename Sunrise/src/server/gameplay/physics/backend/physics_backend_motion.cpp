#include "physics_backend.h"
#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace internal;

/**
 * Queues one validated kinematic pose.
 * @param handle Generation-checked kinematic body.
 * @param target Canonical next-tick target.
 * @return True when the target is valid for the owning scene.
 */
bool PhysicsBackend::set_kinematic_target(BodyHandle handle, const Transform& target) noexcept {
    if (!initialized_ || handle.slot >= bodies_.size()) {
        return false;
    }
    BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation
        || body.spec.motion != MotionKind::kinematic) {
        return false;
    }
    Transform canonical{};
    if (!normalize(target, canonical) || body.spec.scene.slot >= scenes_.size()) {
        return false;
    }
    const SceneSlot& scene = scenes_[body.spec.scene.slot];
    if (!scene.occupied || scene.generation != body.spec.scene.generation
        || !contains(scene.spec.bounds, proxy_bounds(body.spec.shape, canonical))) {
        return false;
    }
    body.kinematicTarget = canonical;
    body.hasKinematicTarget = true;
    body.kinematicVelocityDriven = false;
    return true;
}

/**
 * Moves one body without creating a swept contact.
 * @param handle Generation-checked body.
 * @param transform Canonical destination pose.
 * @return True when the pose fits the owning scene.
 */
bool PhysicsBackend::teleport_body(BodyHandle handle, const Transform& transform) noexcept {
    if (!initialized_ || handle.slot >= bodies_.size()) {
        return false;
    }
    BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation
        || body.spec.scene.slot >= scenes_.size()) {
        return false;
    }
    Transform canonical{};
    const SceneSlot& scene = scenes_[body.spec.scene.slot];
    if (!scene.occupied || scene.generation != body.spec.scene.generation
        || !normalize(transform, canonical)
        || !contains(scene.spec.bounds, proxy_bounds(body.spec.shape, canonical))
        || !can_advance(body.revision)) {
        return false;
    }
    body.spec.transform = canonical;
    body.previousTransform = canonical;
    body.kinematicTarget = canonical;
    body.hasKinematicTarget = false;
    body.kinematicVelocityDriven = false;
    ++body.revision;
    discard_contacts(handle);
    return true;
}

/**
 * Replaces canonical velocity for one movable body.
 * @param handle Generation-checked body.
 * @param linear World linear velocity.
 * @param angular World angular velocity.
 * @return True when the body and both vectors are valid.
 */
bool PhysicsBackend::set_velocity(BodyHandle handle, Vec3 linear, Vec3 angular) noexcept {
    if (!initialized_ || handle.slot >= bodies_.size() || !finite(linear) || !finite(angular)) {
        return false;
    }
    BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation
        || body.spec.motion == MotionKind::staticBody || !can_advance(body.revision)) {
        return false;
    }
    body.spec.linearVelocity = linear;
    body.spec.angularVelocity = angular;
    if (body.spec.motion == MotionKind::kinematic) {
        body.kinematicVelocityDriven = true;
    }
    ++body.revision;
    return true;
}

/**
 * Applies one world impulse through stored inverse mass.
 * @param handle Generation-checked dynamic body.
 * @param impulse Finite world impulse.
 * @return True when the dynamic velocity stays finite.
 */
bool PhysicsBackend::apply_impulse(BodyHandle handle, Vec3 impulse) noexcept {
    if (!initialized_ || handle.slot >= bodies_.size() || !finite(impulse)) {
        return false;
    }
    BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation
        || body.spec.motion != MotionKind::dynamic || body.spec.inverseMass <= 0.0F
        || !can_advance(body.revision)) {
        return false;
    }
    const Vec3 velocity = add(body.spec.linearVelocity, multiply(impulse, body.spec.inverseMass));
    if (!finite(velocity)) {
        return false;
    }
    body.spec.linearVelocity = velocity;
    ++body.revision;
    return true;
}

/**
 * Copies one exact body without exposing storage.
 * @param handle Generation-checked body.
 * @param state Receives canonical body state.
 * @return True when the exact body is live.
 */
bool PhysicsBackend::read_body(BodyHandle handle, BodyState& state) const noexcept {
    if (!initialized_ || handle.slot >= bodies_.size()) {
        return false;
    }
    const BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation) {
        return false;
    }
    BodyState next{};
    next.handle = handle;
    next.spec = body.spec;
    next.previousTransform = body.previousTransform;
    next.kinematicTarget = body.kinematicTarget;
    next.revision = body.revision;
    next.hasKinematicTarget = body.hasKinematicTarget;
    state = next;
    return true;
}

/** @return The number of accepted fixed steps. */
std::uint64_t PhysicsBackend::tick() const noexcept {
    return tick_;
}

} // namespace sunrise::server::gameplay::physics::backend
