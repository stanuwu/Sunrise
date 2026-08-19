#include <cmath>
#include <cstdint>
#include <limits>

#include "tick_runtime.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                                     world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (world::equal_actor(candidate.key, actor)) {
            return &candidate;
        }
    }
    return nullptr;
}

/** Converts one finite world coordinate without integer overflow. */
[[nodiscard]] bool to_millimeters(float value, float scale, std::int32_t& output) noexcept {
    const double converted = static_cast<double>(value) * scale;
    if (!std::isfinite(converted)
        || converted < -static_cast<double>(mechanics::kCombatCoordinateLimitMillimeters)
        || converted > static_cast<double>(mechanics::kCombatCoordinateLimitMillimeters)) {
        return false;
    }
    output = static_cast<std::int32_t>(std::llround(converted));
    return true;
}

[[nodiscard]] bool make_pose(const world::ActorSnapshot& actor,
                             float scale,
                             mechanics::CombatPoseSample& pose,
                             world::TickId tick) noexcept {
    pose.tick = tick;
    pose.authorityEpoch = actor.authority.generation;
    return to_millimeters(actor.transform.position.x, scale, pose.position.x)
           && to_millimeters(actor.transform.position.y, scale, pose.position.y)
           && to_millimeters(actor.transform.position.z, scale, pose.position.z);
}

} // namespace

/** Records canonical poses used by periodic combat and synchronous damage. */
bool record_combat_poses(BubbleHost::Storage::WorldSlot& slot,
                         const world::HostExecutionContext& context,
                         std::span<const world::ActorSnapshot> actors) noexcept {
    for (BubbleHost::Storage::CombatantBinding& binding : slot.combatants) {
        if (!binding.occupied) {
            continue;
        }
        const world::ActorSnapshot* actor = find_actor(actors, binding.actor);
        if (actor == nullptr) {
            continue;
        }
        mechanics::CombatantSnapshot combatant{};
        if (!slot.combat.query(binding.combatant, combatant)) {
            return false;
        }
        if (combatant.authorityEpoch != actor->authority.generation) {
            const mechanics::CombatResult authorityResult =
                slot.combat.set_authority(binding.combatant, actor->authority.generation);
            if (authorityResult != mechanics::CombatResult::applied
                && authorityResult != mechanics::CombatResult::noChange) {
                return false;
            }
        }
        mechanics::CombatPoseSample pose{};
        if (make_pose(*actor, slot.millimetersPerUnit, pose, context.tick)) {
            const mechanics::CombatResult poseResult =
                slot.combat.record_pose(binding.combatant, pose);
            if (poseResult != mechanics::CombatResult::applied
                && poseResult != mechanics::CombatResult::noChange) {
                return false;
            }
        }
    }

    return true;
}

/** Runs periodic combat bookkeeping without replaying command mutations. */
bool run_combat(BubbleHost::Storage::WorldSlot& slot,
                const world::HostExecutionContext& context,
                std::span<const world::ActorSnapshot> actors,
                EventWriter& writer) noexcept {
    static_cast<void>(writer);
    return record_combat_poses(slot, context, actors);
}

} // namespace sunrise::server::gameplay::physics::host::detail
