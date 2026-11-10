#pragma once

#include <algorithm>

#include "../../../middleware/bap/activity_message/squad_objective_state.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/**
 * Qualifies reported costs against the exact transported objective and evaluation revision. Only
 * a squad with a retained body has an assignment, so only then are the full bodies copied.
 * @return False when the bodies could not be copied and the instance was faulted.
 */
[[nodiscard]] inline bool qualify_objective_costs(RuntimeInstance& instance,
                                                  host::Event& event) noexcept {
    namespace objective = middleware::bap::activity_message::squad_objective;
    const auto pins = pass_pins(instance);
    if (std::none_of(pins.begin(), pins.end(), [&event](const host::ScriptablePin& pin) noexcept {
            return pin.registryKey == event.firstRegistryKey && pin.objectTag == event.slotObjectTag
                   && pin.slotIndex == event.firstSlotIndex && pin.slotType == event.firstSlotType;
        })) {
        return true;
    }
    if (!copy_estate(instance)) {
        return false;
    }
    for (const auto& retained : instance.estate) {
        const auto& target = retained.target;
        if (target.registryKey != event.firstRegistryKey || target.objectTag != event.slotObjectTag
            || target.slotIndex != event.firstSlotIndex || target.slotType != event.firstSlotType) {
            continue;
        }
        objective::State assignment{};
        if (objective::read_state(std::span(retained.body).first(retained.byteCount),
                                  retained.bitCount,
                                  assignment)) {
            event.squadObjectiveRegistryKey = assignment.registryKey;
            event.squadObjectiveSlotIndex = assignment.objectiveIndex;
            event.squadObjectiveTaskGroup = assignment.taskGroup;
            event.squadObjectiveCostQualified =
                event.squadObjectiveRevision != 0
                && assignment.revision == event.squadObjectiveRevision;
        }
        return true;
    }
    return true;
}

} // namespace sunrise::server::activity::mission
