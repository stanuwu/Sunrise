#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "physics_backend.h"
#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend {

using namespace internal;

/**
 * Intersects one finite ray and writes stable-order hits.
 * @param query Exact scene, owner, gate, and ray input.
 * @param hits Receives results only after full validation.
 * @return Explicit query or context status.
 */
QueryStatus PhysicsBackend::raycast(const RayQuery& query, QueryHitBuffer& hits) const noexcept {
    if (!initialized_ || query.scene.slot >= scenes_.size()) {
        return QueryStatus::staleScene;
    }
    const SceneSlot& scene = scenes_[query.scene.slot];
    if (!scene.occupied || scene.generation != query.scene.generation) {
        return QueryStatus::staleScene;
    }
    if (query.gate.bubble != scene.spec.bubble) {
        return QueryStatus::contextMismatch;
    }
    const double directionLength = length(query.direction);
    if (query.stableOwnerId == 0 || query.shapeId == 0 || query.gate.layer >= kCollisionLayerCount
        || !finite(query.origin) || !finite(query.direction) || !finite(query.maxDistance)
        || query.maxDistance < 0.0F || !std::isfinite(directionLength)
        || directionLength <= kMinimumMagnitude) {
        return QueryStatus::invalidQuery;
    }
    const Vec3 direction = multiply(query.direction, static_cast<float>(1.0 / directionLength));
    QueryHitBuffer next{};
    for (std::size_t index = 0; index < bodies_.size(); ++index) {
        const BodySlot& body = bodies_[index];
        if (!body.occupied || body.spec.scene != query.scene
            || body.spec.stableOwnerId == query.ignoreOwnerId
            || (!query.includeTriggers && body.spec.isTrigger)
            || !gate_matches(query.gate, body.spec.gate)) {
            continue;
        }
        float distance = 0.0F;
        Vec3 normal{};
        if (!ray_aabb(query.origin,
                      direction,
                      query.maxDistance,
                      proxy_bounds(body.spec.shape, body.spec.transform),
                      distance,
                      normal)) {
            continue;
        }
        QueryHit hit{};
        hit.body = {static_cast<std::uint16_t>(index), body.generation};
        hit.stableOwnerId = body.spec.stableOwnerId;
        hit.shapeId = body.spec.shapeId;
        hit.distance = distance;
        hit.fraction = query.maxDistance > 0.0F ? distance / query.maxDistance : 0.0F;
        hit.position = add(query.origin, multiply(direction, distance));
        hit.normal = normal;
        hit.isTrigger = body.spec.isTrigger;
        insert_hit(next, hit);
    }
    hits = next;
    return QueryStatus::success;
}

/**
 * Sweeps one conservative proxy and writes stable-order hits.
 * @param query Exact scene, owner, gate, shape, and motion input.
 * @param hits Receives results only after full validation.
 * @return Explicit query or context status.
 */
QueryStatus PhysicsBackend::sweep(const SweepQuery& query, QueryHitBuffer& hits) const noexcept {
    if (!initialized_ || query.scene.slot >= scenes_.size()) {
        return QueryStatus::staleScene;
    }
    const SceneSlot& scene = scenes_[query.scene.slot];
    if (!scene.occupied || scene.generation != query.scene.generation) {
        return QueryStatus::staleScene;
    }
    if (query.gate.bubble != scene.spec.bubble) {
        return QueryStatus::contextMismatch;
    }
    Transform canonical{};
    if (query.stableOwnerId == 0 || query.shapeId == 0 || query.gate.layer >= kCollisionLayerCount
        || !valid(query.shape) || !normalize(query.transform, canonical)
        || !finite(query.displacement)) {
        return QueryStatus::invalidQuery;
    }
    const Aabb source = proxy_bounds(query.shape, canonical);
    const Vec3 sourceCenter = center(source);
    const Vec3 sourceExtents = half_extents(source);
    const double travelLength = length(query.displacement);
    if (!std::isfinite(travelLength)
        || travelLength > static_cast<double>(std::numeric_limits<float>::max())) {
        return QueryStatus::invalidQuery;
    }
    const Vec3 direction =
        travelLength > kMinimumMagnitude
            ? multiply(query.displacement, static_cast<float>(1.0 / travelLength))
            : Vec3{};
    QueryHitBuffer next{};
    for (std::size_t index = 0; index < bodies_.size(); ++index) {
        const BodySlot& body = bodies_[index];
        if (!body.occupied || body.spec.scene != query.scene
            || body.spec.stableOwnerId == query.ignoreOwnerId
            || (!query.includeTriggers && body.spec.isTrigger)
            || !gate_matches(query.gate, body.spec.gate)) {
            continue;
        }
        const Aabb target = proxy_bounds(body.spec.shape, body.spec.transform);
        float distance = 0.0F;
        Vec3 normal{};
        bool hitTarget = overlaps(source, target);
        if (hitTarget) {
            normal = overlap_normal(target, source);
        } else if (travelLength > kMinimumMagnitude) {
            hitTarget = ray_aabb(sourceCenter,
                                 direction,
                                 static_cast<float>(travelLength),
                                 expanded(target, sourceExtents),
                                 distance,
                                 normal);
        }
        if (!hitTarget) {
            continue;
        }
        QueryHit hit{};
        hit.body = {static_cast<std::uint16_t>(index), body.generation};
        hit.stableOwnerId = body.spec.stableOwnerId;
        hit.shapeId = body.spec.shapeId;
        hit.distance = distance;
        hit.fraction = travelLength > kMinimumMagnitude
                           ? static_cast<float>(static_cast<double>(distance) / travelLength)
                           : 0.0F;
        hit.position = add(sourceCenter, multiply(direction, distance));
        hit.normal = normal;
        hit.isTrigger = body.spec.isTrigger;
        insert_hit(next, hit);
    }
    hits = next;
    return QueryStatus::success;
}

/**
 * Overlaps one conservative proxy and writes stable-order hits.
 * @param query Exact scene, owner, gate, shape, and pose input.
 * @param hits Receives results only after full validation.
 * @return Explicit query or context status.
 */
QueryStatus PhysicsBackend::overlap(const OverlapQuery& query,
                                    QueryHitBuffer& hits) const noexcept {
    if (!initialized_ || query.scene.slot >= scenes_.size()) {
        return QueryStatus::staleScene;
    }
    const SceneSlot& scene = scenes_[query.scene.slot];
    if (!scene.occupied || scene.generation != query.scene.generation) {
        return QueryStatus::staleScene;
    }
    if (query.gate.bubble != scene.spec.bubble) {
        return QueryStatus::contextMismatch;
    }
    Transform canonical{};
    if (query.stableOwnerId == 0 || query.shapeId == 0 || query.gate.layer >= kCollisionLayerCount
        || !valid(query.shape) || !normalize(query.transform, canonical)) {
        return QueryStatus::invalidQuery;
    }
    const Aabb source = proxy_bounds(query.shape, canonical);
    QueryHitBuffer next{};
    for (std::size_t index = 0; index < bodies_.size(); ++index) {
        const BodySlot& body = bodies_[index];
        if (!body.occupied || body.spec.scene != query.scene
            || body.spec.stableOwnerId == query.ignoreOwnerId
            || (!query.includeTriggers && body.spec.isTrigger)
            || !gate_matches(query.gate, body.spec.gate)) {
            continue;
        }
        const Aabb target = proxy_bounds(body.spec.shape, body.spec.transform);
        if (!overlaps(source, target)) {
            continue;
        }
        QueryHit hit{};
        hit.body = {static_cast<std::uint16_t>(index), body.generation};
        hit.stableOwnerId = body.spec.stableOwnerId;
        hit.shapeId = body.spec.shapeId;
        hit.position = overlap_point(source, target);
        hit.normal = overlap_normal(target, source);
        hit.isTrigger = body.spec.isTrigger;
        insert_hit(next, hit);
    }
    hits = next;
    return QueryStatus::success;
}

} // namespace sunrise::server::gameplay::physics::backend
