#pragma once

#include <limits>

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "activity_sdk_mission_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {

/**
 * Collects only the selected scene descriptor's exact squad edges.
 * @param catalog Authenticated SDK catalog.
 * @param sceneSlot Owned scene slot.
 * @param resource Selected resource descriptor for that slot.
 * @param output Receives the complete dependency set; cleared on failure.
 * @return Ready for an exact bounded set, including an empty set.
 */
[[nodiscard]] inline SceneStatus
scene_dependencies(const state::activity_sdk::Catalog& catalog,
                   const state::activity_sdk::format::Slot& sceneSlot,
                   const state::activity_sdk::format::AuthoredSceneResource& resource,
                   middleware::bap::activity_message::sensor_auth_update::AuthoredSceneDependencies&
                       output) noexcept {
    namespace sdk = state::activity_sdk;
    namespace message = middleware::bap::activity_message::sensor_auth_update;
    output = {};
    message::AuthoredSceneDependencies candidate{};
    const auto slots = catalog.slots();
    if (resource.slotIndex >= slots.size() || &slots[resource.slotIndex] != &sceneSlot
        || sceneSlot.slotType != sdk::format::kAuthoredSceneSlotType
        || sceneSlot.componentClass != sdk::format::kAuthoredSceneComponentClass
        || sceneSlot.authSchema != sdk::format::kAuthoredSceneAuthSchema
        || sceneSlot.senseSchema != sdk::format::kAuthoredSceneSenseSchema
        || resource.flags != sdk::format::kAuthoredSceneResourceExact) {
        return SceneStatus::invalidSlot;
    }
    const auto edges = sdk::slot_authored_scene_squad_edges(catalog, sceneSlot);
    if (edges.size() > candidate.references.size()) {
        return SceneStatus::refused;
    }
    for (const auto& edge : edges) {
        const auto* target = sdk::authored_scene_linked_squad_slot(catalog, edge);
        if (edge.flags != sdk::format::kAuthoredSceneSquadSameObjectExact
            || edge.configTag != resource.configTag
            || edge.descriptorOffset != resource.descriptorOffset || target == nullptr
            || target->objectIndex != sceneSlot.objectIndex
            || target->objectIndex >= catalog.objects().size()
            || edge.targetObjectKey != catalog.objects()[target->objectIndex].objectKey
            || target->slotType != sdk::format::kSquadSlotType
            || target->componentClass != sdk::format::kSquadComponentClass
            || target->authSchema != sdk::format::kSquadAuthSchema
            || target->senseSchema != sdk::format::kSquadSenseSchema
            || (target->flags & sdk::format::kSlotSchemaJoinExact) == 0
            || target->slotIndex
                   > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
            return SceneStatus::ambiguousTarget;
        }
        candidate.references[candidate.count++] = {edge.targetObjectKey,
                                                   static_cast<std::int8_t>(target->slotType),
                                                   static_cast<std::int16_t>(target->slotIndex)};
    }
    if (!message::valid_authored_scene_dependencies(candidate)) {
        return SceneStatus::ambiguousTarget;
    }
    output = candidate;
    return SceneStatus::ready;
}

} // namespace sunrise::server::activity::activity_sdk_mission
