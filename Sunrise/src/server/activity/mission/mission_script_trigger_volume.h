#pragma once

#include <array>

#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::server::activity::mission::trigger_volume {

/**
 * @return True when `point` lies inside one type-60 volume instance: above a triangle of its
 * floor footprint and no higher than that floor plus the instance's extrusion. Inactive or
 * incomplete instances contain nothing.
 */
[[nodiscard]] bool contains(const state::build_data::scriptables::Snapshot& world,
                            const state::build_data::scriptables::TriggerVolumeInstance& volume,
                            const std::array<float, 3>& point) noexcept;

} // namespace sunrise::server::activity::mission::trigger_volume
