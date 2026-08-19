#pragma once

#include "types.h"

namespace sunrise::server::gameplay::physics::backend::internal {

/** Query and integration vectors below this magnitude are treated as zero. */
inline constexpr float kMinimumMagnitude = 1.0E-6F;

[[nodiscard]] bool finite(float value) noexcept;
[[nodiscard]] bool finite(Vec3 value) noexcept;
[[nodiscard]] bool finite(Quaternion value) noexcept;
[[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] Vec3 multiply(Vec3 value, float scale) noexcept;
[[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] double length(Vec3 value) noexcept;
[[nodiscard]] bool nonzero(Vec3 value) noexcept;
[[nodiscard]] bool valid(MotionKind motion) noexcept;
[[nodiscard]] bool can_advance(std::uint64_t value) noexcept;
[[nodiscard]] std::uint32_t next_generation(std::uint32_t generation) noexcept;
[[nodiscard]] bool generation_reusable(std::uint32_t generation) noexcept;
[[nodiscard]] bool normalize(Quaternion input, Quaternion& output) noexcept;
[[nodiscard]] bool normalize(Transform input, Transform& output) noexcept;
[[nodiscard]] bool valid(const Aabb& bounds) noexcept;
[[nodiscard]] bool contains(const Aabb& outer, const Aabb& inner) noexcept;
[[nodiscard]] bool valid(const Shape& shape) noexcept;
[[nodiscard]] Vec3 rotate(Quaternion rotation, Vec3 value) noexcept;
[[nodiscard]] Vec3 proxy_extents(const Shape& shape, Quaternion rotation) noexcept;
[[nodiscard]] Aabb proxy_bounds(const Shape& shape, const Transform& transform) noexcept;
[[nodiscard]] bool overlaps(const Aabb& left, const Aabb& right) noexcept;
[[nodiscard]] Vec3 center(const Aabb& bounds) noexcept;
[[nodiscard]] Vec3 half_extents(const Aabb& bounds) noexcept;
[[nodiscard]] Aabb expanded(const Aabb& bounds, Vec3 amount) noexcept;
[[nodiscard]] bool gate_matches(const CollisionGate& left, const CollisionGate& right) noexcept;
[[nodiscard]] bool ray_aabb(Vec3 origin,
                            Vec3 direction,
                            float maxDistance,
                            const Aabb& bounds,
                            float& distance,
                            Vec3& normal) noexcept;
[[nodiscard]] Vec3 overlap_normal(const Aabb& first, const Aabb& second) noexcept;
[[nodiscard]] Vec3 overlap_point(const Aabb& first, const Aabb& second) noexcept;
void insert_hit(QueryHitBuffer& output, const QueryHit& hit) noexcept;
void insert_contact(ContactBuffer& output, const Contact& contact) noexcept;
[[nodiscard]] bool body_key_less(std::uint64_t leftOwner,
                                 std::uint32_t leftShape,
                                 std::uint16_t leftSlot,
                                 std::uint64_t rightOwner,
                                 std::uint32_t rightShape,
                                 std::uint16_t rightSlot) noexcept;
[[nodiscard]] Quaternion
integrated_rotation(Quaternion rotation, Vec3 angularVelocity, float fixedDt) noexcept;
void clamp_to_bounds(const Aabb& bounds, const Transform& fallback, BodySpec& body) noexcept;

} // namespace sunrise::server::gameplay::physics::backend::internal
