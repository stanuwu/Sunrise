#pragma once

#include <algorithm>
#include <span>

#include "../../../middleware/bap/activity_message/squad_objective_state.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {

/**
 * Reads the objective assignment last transported to one squad slot.
 * @return False when the estate holds no readable body for the slot.
 */
[[nodiscard]] inline bool
transported_objective(std::span<const host::PendingScriptableOverride> estate,
                      std::uint32_t registryKey,
                      std::uint32_t objectTag,
                      std::uint16_t slotIndex,
                      std::uint32_t slotType,
                      middleware::bap::activity_message::squad_objective::State& output) noexcept {
    for (const auto& retained : estate) {
        const auto& target = retained.target;
        if (target.registryKey == registryKey && target.objectTag == objectTag
            && target.slotIndex == slotIndex && target.slotType == slotType) {
            return middleware::bap::activity_message::squad_objective::read_state(
                std::span(retained.body).first(retained.byteCount), retained.bitCount, output);
        }
    }
    return false;
}

/**
 * Qualifies reported costs against the exact transported objective and evaluation revision. Only
 * a squad with a retained body has an assignment, so only then are the full bodies copied.
 * @return False when the bodies could not be copied and the instance was faulted.
 */
[[nodiscard]] inline bool qualify_objective_costs(RuntimeInstance& instance,
                                                  host::Event& event) noexcept {
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
    middleware::bap::activity_message::squad_objective::State assignment{};
    if (transported_objective(instance.estate,
                              event.firstRegistryKey,
                              event.slotObjectTag,
                              event.firstSlotIndex,
                              event.firstSlotType,
                              assignment)) {
        event.squadObjectiveRegistryKey = assignment.registryKey;
        event.squadObjectiveSlotIndex = assignment.objectiveIndex;
        event.squadObjectiveTaskGroup = assignment.taskGroup;
        event.squadObjectiveCostQualified = event.squadObjectiveRevision != 0
                                            && assignment.revision == event.squadObjectiveRevision;
    }
    return true;
}

} // namespace sunrise::server::activity::mission
