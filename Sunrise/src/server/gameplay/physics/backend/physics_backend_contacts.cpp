#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "physics_backend.h"
#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace internal;

/**
 * Builds every next contact without changing canonical storage.
 * @param nextBodies Fully validated next body state.
 * @param nextTick Tick assigned only if the caller commits.
 * @param contacts Receives stable-order contacts on success.
 * @return True when every required intermediate remains finite.
 */
bool PhysicsBackend::build_contacts(const std::array<BodySlot, kBodyCapacity>& nextBodies,
                                    std::uint64_t nextTick,
                                    ContactBuffer& contacts) const noexcept {
    ContactBuffer next{};
    next.tick = nextTick;
    for (std::size_t firstIndex = 0; firstIndex < nextBodies.size(); ++firstIndex) {
        const BodySlot& firstRaw = nextBodies[firstIndex];
        if (!firstRaw.occupied) {
            continue;
        }
        for (std::size_t secondIndex = firstIndex + 1; secondIndex < nextBodies.size();
             ++secondIndex) {
            const BodySlot& secondRaw = nextBodies[secondIndex];
            if (!secondRaw.occupied || firstRaw.spec.scene != secondRaw.spec.scene
                || !gate_matches(firstRaw.spec.gate, secondRaw.spec.gate)) {
                continue;
            }
            const bool rawOrder = body_key_less(firstRaw.spec.stableOwnerId,
                                                firstRaw.spec.shapeId,
                                                static_cast<std::uint16_t>(firstIndex),
                                                secondRaw.spec.stableOwnerId,
                                                secondRaw.spec.shapeId,
                                                static_cast<std::uint16_t>(secondIndex));
            Contact contact{};
            if (!build_contact(rawOrder ? firstRaw : secondRaw,
                               rawOrder ? secondRaw : firstRaw,
                               rawOrder ? firstIndex : secondIndex,
                               rawOrder ? secondIndex : firstIndex,
                               contact)) {
                return false;
            }
            if (contact.firstOwnerId != 0) {
                insert_contact(next, contact);
            }
        }
    }
    contacts = next;
    return true;
}

/**
 * Builds one swept contact from an exact ordered body pair.
 * @param first Stable first body.
 * @param second Stable second body.
 * @param firstSlot First backend slot.
 * @param secondSlot Second backend slot.
 * @param contact Receives a contact, or stays empty when separated.
 * @return True when all intermediate values remain finite.
 */
bool PhysicsBackend::build_contact(const BodySlot& first,
                                   const BodySlot& second,
                                   std::size_t firstSlot,
                                   std::size_t secondSlot,
                                   Contact& contact) const noexcept {
    const Aabb firstBounds = proxy_bounds(first.spec.shape, first.spec.transform);
    const Aabb secondBounds = proxy_bounds(second.spec.shape, second.spec.transform);
    const Aabb firstPrevious = proxy_bounds(first.spec.shape, first.previousTransform);
    const Aabb secondPrevious = proxy_bounds(second.spec.shape, second.previousTransform);
    const Vec3 firstTravel =
        subtract(first.spec.transform.position, first.previousTransform.position);
    const Vec3 secondTravel =
        subtract(second.spec.transform.position, second.previousTransform.position);
    const Vec3 relativeTravel = subtract(firstTravel, secondTravel);
    const double travelLength = length(relativeTravel);
    if (!valid(firstBounds) || !valid(secondBounds) || !valid(firstPrevious)
        || !valid(secondPrevious) || !finite(firstTravel) || !finite(secondTravel)
        || !finite(relativeTravel) || !std::isfinite(travelLength)
        || travelLength > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    const bool startedOverlapping = overlaps(firstPrevious, secondPrevious);
    const bool endedOverlapping = overlaps(firstBounds, secondBounds);
    bool swept = false;
    float fraction = startedOverlapping ? 0.0F : 1.0F;
    Vec3 sweptNormal{};
    if (!startedOverlapping && travelLength > kMinimumMagnitude) {
        const Aabb target = expanded(secondPrevious, half_extents(firstPrevious));
        float distance = 0.0F;
        const Vec3 direction = multiply(relativeTravel, static_cast<float>(1.0 / travelLength));
        if (ray_aabb(center(firstPrevious),
                     direction,
                     static_cast<float>(travelLength),
                     target,
                     distance,
                     sweptNormal)) {
            swept = true;
            fraction = std::clamp(
                static_cast<float>(static_cast<double>(distance) / travelLength), 0.0F, 1.0F);
        }
    }
    if (!startedOverlapping && !endedOverlapping && !swept) {
        return true;
    }
    contact.firstBody = {static_cast<std::uint16_t>(firstSlot), first.generation};
    contact.secondBody = {static_cast<std::uint16_t>(secondSlot), second.generation};
    contact.firstOwnerId = first.spec.stableOwnerId;
    contact.secondOwnerId = second.spec.stableOwnerId;
    contact.firstShapeId = first.spec.shapeId;
    contact.secondShapeId = second.spec.shapeId;
    contact.fraction = fraction;
    if (endedOverlapping) {
        contact.point = overlap_point(firstBounds, secondBounds);
        contact.normal = overlap_normal(firstBounds, secondBounds);
    } else {
        const Vec3 firstAtContact = add(center(firstPrevious), multiply(firstTravel, fraction));
        const Vec3 secondAtContact = add(center(secondPrevious), multiply(secondTravel, fraction));
        contact.point = multiply(add(firstAtContact, secondAtContact), 0.5F);
        contact.normal = multiply(sweptNormal, -1.0F);
    }
    contact.isTrigger = first.spec.isTrigger || second.spec.isTrigger;
    return finite(contact.point) && finite(contact.normal) && finite(contact.fraction);
}

} // namespace sunrise::server::gameplay::physics::backend
