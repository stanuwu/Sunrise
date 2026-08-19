#pragma once

#include "navigation_backend.h"

namespace sunrise::server::gameplay::physics::backend::navigation_internal {

[[nodiscard]] bool finite(float value) noexcept;
[[nodiscard]] bool finite(Vec3 value) noexcept;
[[nodiscard]] std::uint32_t next_generation(std::uint32_t generation) noexcept;
[[nodiscard]] bool generation_reusable(std::uint32_t generation) noexcept;
[[nodiscard]] bool valid(const Shape& shape) noexcept;
[[nodiscard]] float conservative_radius(const Shape& shape) noexcept;
[[nodiscard]] bool inside(const Aabb& bounds, Vec3 point, float radius) noexcept;
[[nodiscard]] bool same_position(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] NavigationStatus map_status(QueryStatus status) noexcept;

} // namespace sunrise::server::gameplay::physics::backend::navigation_internal
