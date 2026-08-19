#pragma once

#include "combat_kernel.h"

namespace sunrise::server::gameplay::physics::mechanics::internal {

[[nodiscard]] bool valid_position(PositionMillimeters position) noexcept;
[[nodiscard]] bool valid_combatant_definition(const CombatantDefinition& definition) noexcept;
[[nodiscard]] bool valid_attack_definition(const AttackBlueprintDefinition& definition) noexcept;
[[nodiscard]] bool equal_position(PositionMillimeters left, PositionMillimeters right) noexcept;
[[nodiscard]] bool
find_pose(const CombatantSnapshot& combatant, TickId tick, CombatPoseSample& sample) noexcept;
[[nodiscard]] bool
within_range(PositionMillimeters source, PositionMillimeters target, std::uint32_t range) noexcept;
[[nodiscard]] TickId saturating_tick_add(TickId tick, TickId amount) noexcept;

} // namespace sunrise::server::gameplay::physics::mechanics::internal
