#include "combat_kernel_internal.h"

#include <array>
#include <limits>

namespace sunrise::server::gameplay::physics::mechanics::internal {

bool valid_position(PositionMillimeters position) noexcept {
    /** Combat positions remain inside the fixed-point range accepted by the kernel. */
    constexpr std::int32_t limit = kCombatCoordinateLimitMillimeters;
    return position.x >= -limit && position.x <= limit && position.y >= -limit
           && position.y <= limit && position.z >= -limit && position.z <= limit;
}

bool valid_combatant_definition(const CombatantDefinition& definition) noexcept {
    return valid_actor_handle(definition.actor) && definition.policyOwnerId != kAbsentPolicyOwnerId
           && definition.faction < 64 && definition.maximumHealth != 0;
}

bool valid_attack_definition(const AttackBlueprintDefinition& definition) noexcept {
    return definition.attackBlueprintId != 0 && definition.policyOwnerId != kAbsentPolicyOwnerId
           && definition.blueprintVersion != 0 && definition.damage != 0
           && definition.maximumRangeMillimeters <= kCombatRangeLimitMillimeters;
}

bool equal_position(PositionMillimeters left, PositionMillimeters right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool find_pose(const CombatantSnapshot& combatant, TickId tick, CombatPoseSample& sample) noexcept {
    for (std::size_t index = 0; index < combatant.rewindSampleCount; ++index) {
        if (combatant.rewindSamples[index].tick == tick) {
            sample = combatant.rewindSamples[index];
            return true;
        }
    }
    return false;
}

/** @return True when two retained positions fit the attack range. */
bool within_range(PositionMillimeters source,
                  PositionMillimeters target,
                  std::uint32_t range) noexcept {
    const std::uint64_t maximumSquared = static_cast<std::uint64_t>(range) * range;
    std::uint64_t accumulated = 0;
    const std::array<std::int64_t, 3> differences{
        static_cast<std::int64_t>(source.x) - target.x,
        static_cast<std::int64_t>(source.y) - target.y,
        static_cast<std::int64_t>(source.z) - target.z,
    };
    for (const std::int64_t difference : differences) {
        const std::uint64_t magnitude =
            static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
        const std::uint64_t squared = magnitude * magnitude;
        if (squared > maximumSquared - accumulated) {
            return false;
        }
        accumulated += squared;
    }
    return true;
}

TickId saturating_tick_add(TickId tick, TickId amount) noexcept {
    const TickId maximum = std::numeric_limits<TickId>::max();
    return amount > maximum - tick ? maximum : tick + amount;
}

} // namespace sunrise::server::gameplay::physics::mechanics::internal
