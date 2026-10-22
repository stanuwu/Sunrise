#include <cstdint>
#include <string_view>

#include "../../../state/activity/destination/activity_destination_spawn_binding.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/build_data/runtime.h"
#include "../../bap/runtime.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/**
 * Moves the client to the region a freshly selected mission state belongs to.
 * The client picks its object registry from the loaded slice-set entry, so a state in another
 * region has no findable objects until it transitions. Message 12 is the only mid-activity move.
 * @param instance Runtime instance whose state selection just published.
 * @param plan Published plan naming the target region and its bubble.
 */
void arm_state_region_teleport(RuntimeInstance& instance,
                               const server::bap::ActivityMissionSeedPlan& plan) noexcept {
    namespace membership = ::sunrise::state::activity::membership;
    const auto& destination = instance.view.binding.destination;
    if (destination.packageNameLength == 0
        || destination.packageNameLength > destination.packageName.size()
        || plan.effectiveRegion > static_cast<std::uint32_t>(membership::kMaximumSliceSetIndex)) {
        return;
    }
    namespace dest = ::sunrise::state::activity::destination;
    // The move carries the spawn set the client filters its spawn points by, not a slice-set name.
    // A set answers only the slice sets of the bubble that declares it; the empty-name hash
    // elsewhere leaves the client on `default`, the set a launch into this region uses.
    const auto target = static_cast<std::uint16_t>(plan.effectiveRegion);
    const std::uint32_t declared = membership::declared_spawn_set(instance.view.binding.sessionId);
    const bool declaredAnswers =
        declared != 0 && dest::spawn_set_slice_set(destination, declared, target) == target;
    const bool armed = server::bap::arm_shared_mission_region(
        instance.view.binding,
        instance.view.activityClientGeneration,
        static_cast<std::int32_t>(plan.effectiveRegion),
        declaredAnswers ? declared : dest::kAbsentSpawnSetHash);
    log_line(core::log::Level::info,
             &instance,
             "state_region",
             armed ? "teleport_armed" : "teleport_unchanged");
}

} // namespace sunrise::server::activity::mission
