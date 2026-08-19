#include <cstdint>

#include "physics_backend.h"
#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace internal;

/** Prepares every next body without changing canonical storage. */
bool PhysicsBackend::prepare_step(float fixedDt,
                                  std::array<BodySlot, kBodyCapacity>& nextBodies) const noexcept {
    nextBodies = bodies_;
    for (BodySlot& body : nextBodies) {
        if (!body.occupied) {
            continue;
        }
        body.previousTransform = body.spec.transform;
        if (body.spec.motion == MotionKind::staticBody) {
            continue;
        }
        if (!valid(body.spec.motion) || !can_advance(body.revision)) {
            return false;
        }
        if (body.spec.motion == MotionKind::kinematic && body.hasKinematicTarget) {
            body.spec.linearVelocity =
                multiply(subtract(body.kinematicTarget.position, body.spec.transform.position),
                         1.0F / fixedDt);
            body.spec.transform = body.kinematicTarget;
            body.hasKinematicTarget = false;
            body.kinematicVelocityDriven = false;
        } else if (body.spec.motion == MotionKind::kinematic && !body.kinematicVelocityDriven) {
            body.spec.linearVelocity = {};
            body.spec.angularVelocity = {};
        } else {
            body.spec.transform.position =
                add(body.spec.transform.position, multiply(body.spec.linearVelocity, fixedDt));
            body.spec.transform.rotation = integrated_rotation(
                body.spec.transform.rotation, body.spec.angularVelocity, fixedDt);
        }
        const SceneSlot& scene = scenes_[body.spec.scene.slot];
        clamp_to_bounds(scene.spec.bounds, body.previousTransform, body.spec);
        if (!finite(body.spec.transform.position) || !finite(body.spec.transform.rotation)
            || !finite(body.spec.linearVelocity) || !finite(body.spec.angularVelocity)) {
            return false;
        }
        ++body.revision;
    }
    return true;
}

/**
 * Advances motion and emits stable-order conservative contacts.
 * @param fixedDt Interval fixed by the first accepted step.
 * @return True when the whole interval validates and commits.
 */
bool PhysicsBackend::step(float fixedDt) noexcept {
    if (!initialized_ || !finite(fixedDt) || fixedDt <= 0.0F || !can_advance(tick_)
        || (fixedDt_ != 0.0F && fixedDt != fixedDt_)) {
        return false;
    }
    std::array<BodySlot, kBodyCapacity> nextBodies{};
    if (!prepare_step(fixedDt, nextBodies)) {
        return false;
    }
    const std::uint64_t nextTick = tick_ + 1U;
    ContactBuffer nextContacts{};
    if (!build_contacts(nextBodies, nextTick, nextContacts)) {
        return false;
    }
    bodies_ = nextBodies;
    fixedDt_ = fixedDt;
    tick_ = nextTick;
    contacts_ = nextContacts;
    return true;
}

/**
 * Drains the last committed step's contact buffer once.
 * @param contacts Receives ordered contacts and overflow count.
 * @return True when the backend is initialized.
 */
bool PhysicsBackend::drain_contacts(ContactBuffer& contacts) noexcept {
    if (!initialized_) {
        return false;
    }
    contacts = contacts_;
    contacts_ = {};
    contacts_.tick = tick_;
    return true;
}

} // namespace sunrise::server::gameplay::physics::backend
