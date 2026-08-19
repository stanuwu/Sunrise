#include <cmath>
#include <cstdint>

#include "physics_backend_internal.h"

namespace sunrise::server::gameplay::physics::backend::internal {

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(Quaternion value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

[[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Vec3 multiply(Vec3 value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] double length(Vec3 value) noexcept {
    return std::hypot(
        static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z));
}

[[nodiscard]] bool nonzero(Vec3 value) noexcept {
    return value.x != 0.0F || value.y != 0.0F || value.z != 0.0F;
}

[[nodiscard]] bool valid(MotionKind motion) noexcept {
    return static_cast<std::uint8_t>(motion) < static_cast<std::uint8_t>(MotionKind::count);
}

[[nodiscard]] bool can_advance(std::uint64_t value) noexcept {
    return value != UINT64_MAX;
}

[[nodiscard]] std::uint32_t next_generation(std::uint32_t generation) noexcept {
    return generation == kExhaustedGeneration ? 0 : generation + 1U;
}

[[nodiscard]] bool generation_reusable(std::uint32_t generation) noexcept {
    return generation != 0 && generation != kExhaustedGeneration;
}

/**
 * Normalizes one finite quaternion.
 * @param input Source rotation.
 * @param output Receives the canonical rotation.
 * @return True when the source has usable magnitude.
 */
[[nodiscard]] bool normalize(Quaternion input, Quaternion& output) noexcept {
    if (!finite(input)) {
        return false;
    }
    const float magnitudeSquared =
        input.x * input.x + input.y * input.y + input.z * input.z + input.w * input.w;
    if (!finite(magnitudeSquared) || magnitudeSquared <= kMinimumMagnitude * kMinimumMagnitude) {
        return false;
    }
    const float inverseMagnitude = 1.0F / std::sqrt(magnitudeSquared);
    output = {input.x * inverseMagnitude,
              input.y * inverseMagnitude,
              input.z * inverseMagnitude,
              input.w * inverseMagnitude};
    return finite(output);
}

/**
 * Normalizes one finite transform.
 * @param input Source pose.
 * @param output Receives the canonical pose.
 * @return True when every component is usable.
 */
[[nodiscard]] bool normalize(Transform input, Transform& output) noexcept {
    if (!finite(input.position)) {
        return false;
    }
    output.position = input.position;
    return normalize(input.rotation, output.rotation);
}

[[nodiscard]] bool valid(const Aabb& bounds) noexcept {
    return finite(bounds.minimum) && finite(bounds.maximum) && bounds.minimum.x <= bounds.maximum.x
           && bounds.minimum.y <= bounds.maximum.y && bounds.minimum.z <= bounds.maximum.z;
}

[[nodiscard]] bool contains(const Aabb& outer, const Aabb& inner) noexcept {
    return inner.minimum.x >= outer.minimum.x && inner.minimum.y >= outer.minimum.y
           && inner.minimum.z >= outer.minimum.z && inner.maximum.x <= outer.maximum.x
           && inner.maximum.y <= outer.maximum.y && inner.maximum.z <= outer.maximum.z;
}

/**
 * Validates one supported conservative proxy.
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
 * Rotates one vector without allocating a matrix.
 * @param rotation Canonical rotation.
 * @param value Source vector.
 * @return Rotated vector.
 */
[[nodiscard]] Vec3 rotate(Quaternion rotation, Vec3 value) noexcept {
    const Vec3 quaternionVector{rotation.x, rotation.y, rotation.z};
    const Vec3 twiceCross{2.0F * (quaternionVector.y * value.z - quaternionVector.z * value.y),
                          2.0F * (quaternionVector.z * value.x - quaternionVector.x * value.z),
                          2.0F * (quaternionVector.x * value.y - quaternionVector.y * value.x)};
    const Vec3 secondCross{quaternionVector.y * twiceCross.z - quaternionVector.z * twiceCross.y,
                           quaternionVector.z * twiceCross.x - quaternionVector.x * twiceCross.z,
                           quaternionVector.x * twiceCross.y - quaternionVector.y * twiceCross.x};
    return add(value, add(multiply(twiceCross, rotation.w), secondCross));
}

/**
 * Gets conservative world axes for one rotated proxy.
 * @param shape Valid proxy.
 * @param rotation Canonical rotation.
 * @return Positive axis extents.
 */
[[nodiscard]] Vec3 proxy_extents(const Shape& shape, Quaternion rotation) noexcept {
    if (shape.kind == ShapeKind::sphere) {
        return {shape.radius, shape.radius, shape.radius};
    }
    if (shape.kind == ShapeKind::capsule) {
        const Vec3 axis = rotate(rotation, {0.0F, 1.0F, 0.0F});
        return {shape.radius + std::abs(axis.x) * shape.halfHeight,
                shape.radius + std::abs(axis.y) * shape.halfHeight,
                shape.radius + std::abs(axis.z) * shape.halfHeight};
    }

    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;
    const float r00 = 1.0F - 2.0F * (y * y + z * z);
    const float r01 = 2.0F * (x * y - z * w);
    const float r02 = 2.0F * (x * z + y * w);
    const float r10 = 2.0F * (x * y + z * w);
    const float r11 = 1.0F - 2.0F * (x * x + z * z);
    const float r12 = 2.0F * (y * z - x * w);
    const float r20 = 2.0F * (x * z - y * w);
    const float r21 = 2.0F * (y * z + x * w);
    const float r22 = 1.0F - 2.0F * (x * x + y * y);
    return {std::abs(r00) * shape.halfExtents.x + std::abs(r01) * shape.halfExtents.y
                + std::abs(r02) * shape.halfExtents.z,
            std::abs(r10) * shape.halfExtents.x + std::abs(r11) * shape.halfExtents.y
                + std::abs(r12) * shape.halfExtents.z,
            std::abs(r20) * shape.halfExtents.x + std::abs(r21) * shape.halfExtents.y
                + std::abs(r22) * shape.halfExtents.z};
}

[[nodiscard]] Aabb proxy_bounds(const Shape& shape, const Transform& transform) noexcept {
    const Vec3 extents = proxy_extents(shape, transform.rotation);
    return {subtract(transform.position, extents), add(transform.position, extents)};
}

[[nodiscard]] bool overlaps(const Aabb& left, const Aabb& right) noexcept {
    return left.minimum.x <= right.maximum.x && left.maximum.x >= right.minimum.x
           && left.minimum.y <= right.maximum.y && left.maximum.y >= right.minimum.y
           && left.minimum.z <= right.maximum.z && left.maximum.z >= right.minimum.z;
}

[[nodiscard]] Vec3 center(const Aabb& bounds) noexcept {
    return multiply(add(bounds.minimum, bounds.maximum), 0.5F);
}

[[nodiscard]] Vec3 half_extents(const Aabb& bounds) noexcept {
    return multiply(subtract(bounds.maximum, bounds.minimum), 0.5F);
}

[[nodiscard]] Aabb expanded(const Aabb& bounds, Vec3 amount) noexcept {
    return {subtract(bounds.minimum, amount), add(bounds.maximum, amount)};
}

} // namespace sunrise::server::gameplay::physics::backend::internal
