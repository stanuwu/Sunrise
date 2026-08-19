#include <cstddef>
#include <cstdint>

#include "physics_backend.h"
#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace internal;

/**
 * Removes pending contacts owned by one exact body.
 * @param handle Body being invalidated.
 */
void PhysicsBackend::discard_contacts(BodyHandle handle) noexcept {
    ContactBuffer kept{};
    kept.tick = contacts_.tick;
    kept.dropped = contacts_.dropped;
    for (std::size_t index = 0; index < contacts_.count; ++index) {
        const Contact& contact = contacts_.contacts[index];
        if (contact.firstBody == handle || contact.secondBody == handle) {
            continue;
        }
        kept.contacts[kept.count] = contact;
        ++kept.count;
    }
    contacts_ = kept;
}

/** Clears fixed storage and advances every handle generation. */
void PhysicsBackend::initialize() noexcept {
    for (SceneSlot& scene : scenes_) {
        const std::uint32_t next = next_generation(scene.generation);
        const std::uint32_t generation = next == 0 ? scene.generation : next;
        scene = {};
        scene.generation = generation;
    }
    for (BodySlot& body : bodies_) {
        const std::uint32_t next = next_generation(body.generation);
        const std::uint32_t generation = next == 0 ? body.generation : next;
        body = {};
        body.generation = generation;
    }
    contacts_ = {};
    tick_ = 0;
    fixedDt_ = 0.0F;
    initialized_ = true;
}

/** Clears fixed storage and leaves the backend unavailable. */
void PhysicsBackend::shutdown() noexcept {
    initialize();
    initialized_ = false;
}

/** @return Capabilities which do not depend on loaded content. */
PhysicsCapabilities PhysicsBackend::capabilities() const noexcept {
    return {};
}

/**
 * Loads one validated build-bound scene.
 * @param spec Complete scene input.
 * @param handle Receives the fixed scene slot.
 * @return True when one free slot accepts the scene.
 */
bool PhysicsBackend::load_scene(const SceneSpec& spec, SceneHandle& handle) noexcept {
    if (!initialized_ || spec.stableSceneId == 0 || spec.contentBuild == 0
        || !finite(spec.coordinateScale) || spec.coordinateScale <= 0.0F || !finite(spec.origin)
        || !valid(spec.bounds)) {
        return false;
    }
    std::size_t freeSlot = scenes_.size();
    for (std::size_t index = 0; index < scenes_.size(); ++index) {
        if (scenes_[index].occupied && scenes_[index].spec.stableSceneId == spec.stableSceneId) {
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
    scene.bodyCount = 0;
    scene.occupied = true;
    handle = {static_cast<std::uint16_t>(freeSlot), scene.generation};
    return true;
}

/**
 * Unloads one exact scene and all owned bodies.
 * @param handle Generation-checked scene.
 * @return True when the exact scene was live.
 */
bool PhysicsBackend::unload_scene(SceneHandle handle) noexcept {
    if (!initialized_ || handle.slot >= scenes_.size()) {
        return false;
    }
    SceneSlot& scene = scenes_[handle.slot];
    if (!scene.occupied || scene.generation != handle.generation) {
        return false;
    }
    for (BodySlot& body : bodies_) {
        if (!body.occupied || body.spec.scene != handle) {
            continue;
        }
        const BodyHandle bodyHandle{static_cast<std::uint16_t>(&body - bodies_.data()),
                                    body.generation};
        discard_contacts(bodyHandle);
        const std::uint32_t next = next_generation(body.generation);
        const std::uint32_t generation = next == 0 ? body.generation : next;
        body = {};
        body.generation = generation;
    }
    const std::uint32_t next = next_generation(scene.generation);
    const std::uint32_t generation = next == 0 ? scene.generation : next;
    scene = {};
    scene.generation = generation;
    return true;
}

/**
 * Copies one exact scene without exposing storage.
 * @param handle Generation-checked scene.
 * @param state Receives scene and body-count state.
 * @return True when the exact scene is live.
 */
bool PhysicsBackend::read_scene(SceneHandle handle, SceneState& state) const noexcept {
    if (!initialized_ || handle.slot >= scenes_.size()) {
        return false;
    }
    const SceneSlot& scene = scenes_[handle.slot];
    if (!scene.occupied || scene.generation != handle.generation) {
        return false;
    }
    SceneState next{};
    next.handle = handle;
    next.spec = scene.spec;
    next.bodyCount = scene.bodyCount;
    state = next;
    return true;
}

/**
 * Validates and stores one conservative body.
 * @param spec Complete body input.
 * @param handle Receives the fixed body slot.
 * @return True when one free slot accepts the body.
 */
bool PhysicsBackend::create_body(const BodySpec& spec, BodyHandle& handle) noexcept {
    if (!initialized_ || spec.scene.slot >= scenes_.size() || spec.stableOwnerId == 0
        || spec.shapeId == 0 || spec.gate.layer >= kCollisionLayerCount || !valid(spec.shape)
        || !finite(spec.linearVelocity) || !finite(spec.angularVelocity)
        || !finite(spec.inverseMass) || !valid(spec.motion)) {
        return false;
    }
    SceneSlot& scene = scenes_[spec.scene.slot];
    if (!scene.occupied || scene.generation != spec.scene.generation
        || scene.spec.bubble != spec.gate.bubble) {
        return false;
    }
    BodySpec canonical = spec;
    if (!normalize(spec.transform, canonical.transform)) {
        return false;
    }
    if (canonical.motion == MotionKind::dynamic) {
        if (canonical.inverseMass <= 0.0F) {
            return false;
        }
    } else {
        canonical.inverseMass = 0.0F;
    }
    if (!contains(scene.spec.bounds, proxy_bounds(canonical.shape, canonical.transform))) {
        return false;
    }

    std::size_t freeSlot = bodies_.size();
    for (std::size_t index = 0; index < bodies_.size(); ++index) {
        const BodySlot& body = bodies_[index];
        if (body.occupied && body.spec.scene == canonical.scene
            && body.spec.stableOwnerId == canonical.stableOwnerId
            && body.spec.shapeId == canonical.shapeId) {
            return false;
        }
        if (!body.occupied && generation_reusable(body.generation) && freeSlot == bodies_.size()) {
            freeSlot = index;
        }
    }
    if (freeSlot == bodies_.size()) {
        return false;
    }

    BodySlot& body = bodies_[freeSlot];
    body.spec = canonical;
    body.previousTransform = canonical.transform;
    body.kinematicTarget = canonical.transform;
    body.revision = 1;
    body.occupied = true;
    body.hasKinematicTarget = false;
    body.kinematicVelocityDriven =
        canonical.motion == MotionKind::kinematic
        && (nonzero(canonical.linearVelocity) || nonzero(canonical.angularVelocity));
    ++scene.bodyCount;
    handle = {static_cast<std::uint16_t>(freeSlot), body.generation};
    return true;
}

/**
 * Destroys one exact body.
 * @param handle Generation-checked body.
 * @return True when the exact body was live.
 */
bool PhysicsBackend::destroy_body(BodyHandle handle) noexcept {
    if (!initialized_ || handle.slot >= bodies_.size()) {
        return false;
    }
    BodySlot& body = bodies_[handle.slot];
    if (!body.occupied || body.generation != handle.generation) {
        return false;
    }
    discard_contacts(handle);
    if (body.spec.scene.slot < scenes_.size()) {
        SceneSlot& scene = scenes_[body.spec.scene.slot];
        if (scene.occupied && scene.generation == body.spec.scene.generation
            && scene.bodyCount > 0) {
            --scene.bodyCount;
        }
    }
    const std::uint32_t next = next_generation(body.generation);
    const std::uint32_t generation = next == 0 ? body.generation : next;
    body = {};
    body.generation = generation;
    return true;
}

} // namespace sunrise::server::gameplay::physics::backend
