#pragma once

#include <array>
#include <cstddef>
#include <limits>

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "activity_sdk_mission_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {

/** One squad participant of a scene: the authored reference and the squad slot it names. */
struct SceneSquadParticipant final {
    middleware::bap::activity_message::sensor_message::ClientReference reference{};
    std::uint32_t squadSlotRow{state::activity_sdk::format::kAbsentIndex};
};

/** Every squad participant of one scene descriptor; the package bounds the table. */
struct SceneSquadParticipants final {
    std::array<SceneSquadParticipant,
               state::activity_sdk::format::kAuthoredSceneParticipantCapacity>
        rows{};
    std::size_t count{};
};

/**
 * Collects the selected scene descriptor's exact squad edges.
 *
 * The client binds every role from the participant table in its own content; these rows only
 * tell the server which squads the scene may draw actors from. The wire dependency set is
 * narrower (see scene_dependencies in activity_sdk_scene_spawn.h).
 * @param catalog Authenticated SDK catalog.
 * @param sceneSlot Owned scene slot.
 * @param resource Selected resource descriptor for that slot.
 * @param output Receives the complete participant set; cleared on failure.
 * @return Ready for an exact set, including an empty set.
 */
[[nodiscard]] inline SceneStatus
scene_squad_participants(const state::activity_sdk::Catalog& catalog,
                         const state::activity_sdk::format::Slot& sceneSlot,
                         const state::activity_sdk::format::AuthoredSceneResource& resource,
                         SceneSquadParticipants& output) noexcept {
    namespace sdk = state::activity_sdk;
    output = {};
    SceneSquadParticipants candidate{};
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
    if (edges.size() > candidate.rows.size()) {
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
        auto& row = candidate.rows[candidate.count++];
        row.reference = {edge.targetObjectKey,
                         static_cast<std::int8_t>(target->slotType),
                         static_cast<std::int16_t>(target->slotIndex)};
        row.squadSlotRow = edge.squadSlotIndex;
    }
    output = candidate;
    return SceneStatus::ready;
}

} // namespace sunrise::server::activity::activity_sdk_mission
