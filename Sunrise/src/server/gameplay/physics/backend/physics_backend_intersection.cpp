#include <algorithm>
#include <cmath>
#include <cstdint>

#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend::internal {

/**
 * Applies the symmetric bubble and layer filter.
 * @param left First collision gate.
 * @param right Second collision gate.
 * @return True when both masks admit the other layer.
 */
[[nodiscard]] bool gate_matches(const CollisionGate& left, const CollisionGate& right) noexcept {
    if (left.bubble != right.bubble || left.layer >= kCollisionLayerCount
        || right.layer >= kCollisionLayerCount) {
        return false;
    }
    const std::uint32_t leftBit = 1U << left.layer;
    const std::uint32_t rightBit = 1U << right.layer;
    return (left.filterMask & rightBit) != 0 && (right.filterMask & leftBit) != 0;
}

/**
 * Selects a stable normal for a ray which starts inside a proxy.
 * @param direction Canonical ray direction.
 * @return Axis normal with x/y/z tie order.
 */
[[nodiscard]] Vec3 initial_normal(Vec3 direction) noexcept {
    const float x = std::abs(direction.x);
    const float y = std::abs(direction.y);
    const float z = std::abs(direction.z);
    if (x >= y && x >= z) {
        return {direction.x >= 0.0F ? -1.0F : 1.0F, 0.0F, 0.0F};
    }
    if (y >= z) {
        return {0.0F, direction.y >= 0.0F ? -1.0F : 1.0F, 0.0F};
    }
    return {0.0F, 0.0F, direction.z >= 0.0F ? -1.0F : 1.0F};
}

/**
 * Intersects one slab and narrows the shared ray interval.
 * @param origin Ray origin on this axis.
 * @param direction Ray direction on this axis.
 * @param minimum Slab minimum.
 * @param maximum Slab maximum.
 * @param minimumNormal Minimum-face normal.
 * @param maximumNormal Maximum-face normal.
 * @param entry Receives the latest entry distance.
 * @param exit Receives the earliest exit distance.
 * @param normal Receives the latest entry normal.
 * @return True when the interval remains nonempty.
 */
[[nodiscard]] bool ray_axis(float origin,
                            float direction,
                            float minimum,
                            float maximum,
                            Vec3 minimumNormal,
                            Vec3 maximumNormal,
                            float& entry,
                            float& exit,
                            Vec3& normal) noexcept {
    if (std::abs(direction) <= kMinimumMagnitude) {
        return origin >= minimum && origin <= maximum;
    }
    float first = (minimum - origin) / direction;
    float second = (maximum - origin) / direction;
    Vec3 firstNormal = minimumNormal;
    Vec3 secondNormal = maximumNormal;
    if (first > second) {
        std::swap(first, second);
        std::swap(firstNormal, secondNormal);
    }
    if (first > entry) {
        entry = first;
        normal = firstNormal;
    }
    exit = std::min(exit, second);
    return entry <= exit;
}

/**
 * Intersects one finite ray with an axis-aligned proxy.
 * @param origin Ray origin.
 * @param direction Unit ray direction.
 * @param maxDistance Finite query distance.
 * @param bounds Target proxy.
 * @param distance Receives the entry distance.
 * @param normal Receives the target-face normal.
 * @return True when the finite ray reaches the proxy.
 */
[[nodiscard]] bool ray_aabb(Vec3 origin,
                            Vec3 direction,
                            float maxDistance,
                            const Aabb& bounds,
                            float& distance,
                            Vec3& normal) noexcept {
    float entry = 0.0F;
    float exit = maxDistance;
    normal = initial_normal(direction);
    if (!ray_axis(origin.x,
                  direction.x,
                  bounds.minimum.x,
                  bounds.maximum.x,
                  {-1.0F, 0.0F, 0.0F},
                  {1.0F, 0.0F, 0.0F},
                  entry,
                  exit,
                  normal)
        || !ray_axis(origin.y,
                     direction.y,
                     bounds.minimum.y,
                     bounds.maximum.y,
                     {0.0F, -1.0F, 0.0F},
                     {0.0F, 1.0F, 0.0F},
                     entry,
                     exit,
                     normal)
        || !ray_axis(origin.z,
                     direction.z,
                     bounds.minimum.z,
                     bounds.maximum.z,
                     {0.0F, 0.0F, -1.0F},
                     {0.0F, 0.0F, 1.0F},
                     entry,
                     exit,
                     normal)) {
        return false;
    }
    distance = entry;
    return entry <= maxDistance && exit >= 0.0F;
}

/**
 * Selects the least-penetration normal with x/y/z tie order.
 * @param first First overlapping proxy.
 * @param second Second overlapping proxy.
 * @return Normal from the first center toward the second.
 */
[[nodiscard]] Vec3 overlap_normal(const Aabb& first, const Aabb& second) noexcept {
    const Vec3 firstCenter = center(first);
    const Vec3 secondCenter = center(second);
    const float x =
        std::min(first.maximum.x, second.maximum.x) - std::max(first.minimum.x, second.minimum.x);
    const float y =
        std::min(first.maximum.y, second.maximum.y) - std::max(first.minimum.y, second.minimum.y);
    const float z =
        std::min(first.maximum.z, second.maximum.z) - std::max(first.minimum.z, second.minimum.z);
    if (x <= y && x <= z) {
        return {secondCenter.x >= firstCenter.x ? 1.0F : -1.0F, 0.0F, 0.0F};
    }
    if (y <= z) {
        return {0.0F, secondCenter.y >= firstCenter.y ? 1.0F : -1.0F, 0.0F};
    }
    return {0.0F, 0.0F, secondCenter.z >= firstCenter.z ? 1.0F : -1.0F};
}

/**
 * Finds the center of one proxy intersection.
 * @param first First overlapping proxy.
 * @param second Second overlapping proxy.
 * @return Stable intersection center.
 */
[[nodiscard]] Vec3 overlap_point(const Aabb& first, const Aabb& second) noexcept {
    return {
        (std::max(first.minimum.x, second.minimum.x) + std::min(first.maximum.x, second.maximum.x))
            * 0.5F,
        (std::max(first.minimum.y, second.minimum.y) + std::min(first.maximum.y, second.maximum.y))
            * 0.5F,
        (std::max(first.minimum.z, second.minimum.z) + std::min(first.maximum.z, second.maximum.z))
            * 0.5F};
}

} // namespace sunrise::server::gameplay::physics::backend::internal
