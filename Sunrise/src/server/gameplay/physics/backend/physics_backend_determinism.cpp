#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend::internal {

/**
 * Compares query hits by time, owner, shape, then slot.
 * @param left First hit.
 * @param right Second hit.
 * @return True when the first hit sorts earlier.
 */
[[nodiscard]] bool hit_less(const QueryHit& left, const QueryHit& right) noexcept {
    if (left.fraction != right.fraction) {
        return left.fraction < right.fraction;
    }
    if (left.stableOwnerId != right.stableOwnerId) {
        return left.stableOwnerId < right.stableOwnerId;
    }
    if (left.shapeId != right.shapeId) {
        return left.shapeId < right.shapeId;
    }
    return left.body.slot < right.body.slot;
}

/**
 * Inserts one hit into fixed stable-order output.
 * @param output Bounded query output.
 * @param hit Hit to insert.
 */
void insert_hit(QueryHitBuffer& output, const QueryHit& hit) noexcept {
    if (output.count == output.hits.size()) {
        output.truncated = true;
        return;
    }
    std::size_t position = output.count;
    while (position > 0 && hit_less(hit, output.hits[position - 1])) {
        output.hits[position] = output.hits[position - 1];
        --position;
    }
    output.hits[position] = hit;
    ++output.count;
}

/**
 * Compares contacts by time and both stable body keys.
 * @param left First contact.
 * @param right Second contact.
 * @return True when the first contact sorts earlier.
 */
[[nodiscard]] bool contact_less(const Contact& left, const Contact& right) noexcept {
    if (left.fraction != right.fraction) {
        return left.fraction < right.fraction;
    }
    if (left.firstOwnerId != right.firstOwnerId) {
        return left.firstOwnerId < right.firstOwnerId;
    }
    if (left.firstShapeId != right.firstShapeId) {
        return left.firstShapeId < right.firstShapeId;
    }
    if (left.secondOwnerId != right.secondOwnerId) {
        return left.secondOwnerId < right.secondOwnerId;
    }
    if (left.secondShapeId != right.secondShapeId) {
        return left.secondShapeId < right.secondShapeId;
    }
    if (left.firstBody.slot != right.firstBody.slot) {
        return left.firstBody.slot < right.firstBody.slot;
    }
    return left.secondBody.slot < right.secondBody.slot;
}

/**
 * Keeps the earliest fixed-capacity contacts in stable order.
 * @param output Bounded contact output.
 * @param contact Contact to insert.
 */
void insert_contact(ContactBuffer& output, const Contact& contact) noexcept {
    if (output.count == output.contacts.size()) {
        ++output.dropped;
        if (!contact_less(contact, output.contacts[output.count - 1])) {
            return;
        }
        output.contacts[output.count - 1] = contact;
    } else {
        output.contacts[output.count] = contact;
        ++output.count;
    }
    std::size_t position = output.count - 1;
    while (position > 0 && contact_less(output.contacts[position], output.contacts[position - 1])) {
        std::swap(output.contacts[position], output.contacts[position - 1]);
        --position;
    }
}

/**
 * Compares actor-local body keys without backend generations.
 * @param leftOwner Left stable owner.
 * @param leftShape Left actor-local shape.
 * @param leftSlot Left backend slot tie-breaker.
 * @param rightOwner Right stable owner.
 * @param rightShape Right actor-local shape.
 * @param rightSlot Right backend slot tie-breaker.
 * @return True when the left owner, shape, and slot sort earlier.
 */
[[nodiscard]] bool body_key_less(std::uint64_t leftOwner,
                                 std::uint32_t leftShape,
                                 std::uint16_t leftSlot,
                                 std::uint64_t rightOwner,
                                 std::uint32_t rightShape,
                                 std::uint16_t rightSlot) noexcept {
    if (leftOwner != rightOwner) {
        return leftOwner < rightOwner;
    }
    if (leftShape != rightShape) {
        return leftShape < rightShape;
    }
    return leftSlot < rightSlot;
}

/**
 * Integrates angular velocity and restores unit quaternion length.
 * @param rotation Canonical current rotation.
 * @param angularVelocity World angular velocity.
 * @param fixedDt Accepted fixed interval.
 * @return Canonical next rotation.
 */
[[nodiscard]] Quaternion
integrated_rotation(Quaternion rotation, Vec3 angularVelocity, float fixedDt) noexcept {
    Quaternion result{
        rotation.x
            + 0.5F
                  * (angularVelocity.x * rotation.w + angularVelocity.y * rotation.z
                     - angularVelocity.z * rotation.y)
                  * fixedDt,
        rotation.y
            + 0.5F
                  * (-angularVelocity.x * rotation.z + angularVelocity.y * rotation.w
                     + angularVelocity.z * rotation.x)
                  * fixedDt,
        rotation.z
            + 0.5F
                  * (angularVelocity.x * rotation.y - angularVelocity.y * rotation.x
                     + angularVelocity.z * rotation.w)
                  * fixedDt,
        rotation.w
            - 0.5F
                  * (angularVelocity.x * rotation.x + angularVelocity.y * rotation.y
                     + angularVelocity.z * rotation.z)
                  * fixedDt,
    };
    Quaternion canonical{};
    return normalize(result, canonical) ? canonical : rotation;
}

/**
 * Keeps one conservative proxy inside its scene bounds.
 * @param bounds Exact scene bounds.
 * @param fallback Last accepted pose.
 * @param body Body state to clamp.
 */
void clamp_to_bounds(const Aabb& bounds, const Transform& fallback, BodySpec& body) noexcept {
    Vec3 extents = proxy_extents(body.shape, body.transform.rotation);
    Vec3 minimum = add(bounds.minimum, extents);
    Vec3 maximum = subtract(bounds.maximum, extents);
    if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
        body.transform.rotation = fallback.rotation;
        extents = proxy_extents(body.shape, body.transform.rotation);
        minimum = add(bounds.minimum, extents);
        maximum = subtract(bounds.maximum, extents);
        body.angularVelocity = {};
    }
    const Vec3 before = body.transform.position;
    body.transform.position.x = std::clamp(before.x, minimum.x, maximum.x);
    body.transform.position.y = std::clamp(before.y, minimum.y, maximum.y);
    body.transform.position.z = std::clamp(before.z, minimum.z, maximum.z);
    if (body.transform.position.x != before.x) {
        body.linearVelocity.x = 0.0F;
    }
    if (body.transform.position.y != before.y) {
        body.linearVelocity.y = 0.0F;
    }
    if (body.transform.position.z != before.z) {
        body.linearVelocity.z = 0.0F;
    }
}

} // namespace sunrise::server::gameplay::physics::backend::internal
