#include <limits>

#include "../../middleware/gameplay/external/actor_entity_registry.h"
#include "activity_sdk_scene_dependencies.h"
#include "activity_sdk_scene_spawn.h"

namespace sunrise::server::activity::activity_sdk_mission {
namespace {

namespace sdk = state::activity_sdk;
namespace auth = middleware::bap::activity_message::scriptable_auth;
using Source = state::gameplay::entity_identity::ActorSourceReference;

/** Only exact Type 2 rows may enter a scene's inverse parent join. */
[[nodiscard]] bool exact_actor(const sdk::format::Slot& actor) noexcept {
    return actor.slotType == auth::kType2SlotType
           && actor.componentClass == auth::kType2ComponentClass
           && actor.authSchema == auth::kType2Schema && actor.senseSchema == auth::kType2SenseSchema
           && (actor.flags & sdk::format::kSlotSchemaJoinExact) != 0
           && actor.slotIndex <= (std::numeric_limits<std::uint16_t>::max)();
}

/** A source must name one squad occurrence in the scene's own object and state. */
[[nodiscard]] SceneStatus source_squad(const sdk::Catalog& catalog,
                                       std::uint32_t occurrenceRow,
                                       const Source& source,
                                       std::uint32_t& output) noexcept {
    output = sdk::format::kAbsentIndex;
    const auto& occurrence = catalog.occurrences()[occurrenceRow];
    for (std::size_t index = 0; index < catalog.squads().size(); ++index) {
        const auto& squad = catalog.squads()[index];
        if (squad.scenarioIndex != occurrence.scenarioIndex
            || squad.objectIndex != occurrence.objectIndex
            || squad.occurrenceIndex >= catalog.occurrences().size()
            || squad.slotIndex >= catalog.slots().size()) {
            continue;
        }
        const auto& sourceOccurrence = catalog.occurrences()[squad.occurrenceIndex];
        if (sourceOccurrence.scenarioIndex != occurrence.scenarioIndex
            || sourceOccurrence.stateIndex != occurrence.stateIndex
            || sourceOccurrence.objectIndex != occurrence.objectIndex) {
            continue;
        }
        const auto& slot = catalog.slots()[squad.slotIndex];
        if (slot.slotIndex != source.index || slot.slotType != source.type) {
            continue;
        }
        if (output != sdk::format::kAbsentIndex) {
            return SceneStatus::ambiguousTarget;
        }
        output = static_cast<std::uint32_t>(index);
    }
    return output == sdk::format::kAbsentIndex ? SceneStatus::targetUnavailable
                                               : SceneStatus::ready;
}

/** Retained ownership uses physical identity without a publication route. */
[[nodiscard]] host::ScriptableTarget physical_target(const sdk::Catalog& catalog,
                                                     std::uint32_t slotRow) noexcept {
    const auto& slot = catalog.slots()[slotRow];
    const auto& object = catalog.objects()[slot.objectIndex];
    return {.objectTag = object.objectTag,
            .registryKey = object.objectKey,
            .authSchema = slot.authSchema,
            .slotIndex = static_cast<std::uint16_t>(slot.slotIndex),
            .slotType = static_cast<std::uint8_t>(slot.slotType)};
}

} // namespace

/**
 * Resolves the complete cast before exposing any pair.
 *
 * A squad participant joins the cast only when one exact actor control in the scene's own
 * scenario state resolves to it. The client binds every role from its content and waits for
 * the squads nobody supplies, so a participant without an actor control is left to it rather
 * than refused.
 * @param catalog Authenticated SDK data.
 * @param world Exact generated package references.
 * @param occurrenceRow Selected scene occurrence.
 * @param sceneSlotRow Type 43 slot owned by that occurrence.
 * @param output Receives the complete plan; cleared on failure.
 * @return Ready only when every cast squad has exactly one actor control.
 */
SceneStatus collect_scene_spawn_plan(const sdk::Catalog& catalog,
                                     const state::build_data::scriptables::Snapshot& world,
                                     std::uint32_t occurrenceRow,
                                     std::uint32_t sceneSlotRow,
                                     SceneSpawnPlan& output) noexcept {
    output = {};
    const auto occurrences = catalog.occurrences();
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    if (occurrenceRow >= occurrences.size() || sceneSlotRow >= slots.size()) {
        return SceneStatus::invalidOccurrence;
    }
    const auto& occurrence = occurrences[occurrenceRow];
    const auto& scene = slots[sceneSlotRow];
    if (occurrence.objectIndex >= objects.size() || scene.objectIndex != occurrence.objectIndex) {
        return SceneStatus::invalidSlot;
    }
    const auto resources = sdk::slot_authored_scene_resources(catalog, scene);
    if (resources.empty()) {
        return SceneStatus::missingResource;
    }
    if (resources.size() != 1) {
        return SceneStatus::ambiguousResource;
    }
    SceneSquadParticipants participants{};
    const auto resolved = scene_squad_participants(catalog, scene, resources.front(), participants);
    if (resolved != SceneStatus::ready) {
        return resolved;
    }
    SceneSpawnPlan candidate{};
    for (std::size_t index = 0; index < participants.count; ++index) {
        const auto& reference = participants.rows[index].reference;
        const Source source{reference.rosterKey,
                            static_cast<std::uint16_t>(reference.slotIndex),
                            static_cast<std::uint8_t>(reference.slotType),
                            true,
                            true};
        auto& pair = candidate.pairs[candidate.count];
        const auto parent = source_squad(catalog, occurrenceRow, source, pair.squadRow);
        if (parent == SceneStatus::targetUnavailable) {
            pair = {};
            continue;
        }
        if (parent != SceneStatus::ready) {
            return parent;
        }
        for (const auto& actorOccurrence : occurrences) {
            if (actorOccurrence.scenarioIndex != occurrence.scenarioIndex
                || actorOccurrence.stateIndex != occurrence.stateIndex) {
                continue;
            }
            if (actorOccurrence.objectIndex >= objects.size()) {
                return SceneStatus::invalidOccurrence;
            }
            const auto& object = objects[actorOccurrence.objectIndex];
            for (const auto& actor : sdk::object_slots(catalog, object)) {
                if (!exact_actor(actor)) {
                    continue;
                }
                const auto actorIndex = static_cast<std::uint32_t>(&actor - slots.data());
                if (pair.actorSlotRow == actorIndex) {
                    continue;
                }
                const Source actorSource{object.objectKey,
                                         static_cast<std::uint16_t>(actor.slotIndex),
                                         static_cast<std::uint8_t>(actor.slotType),
                                         true,
                                         true};
                Source parentSource{};
                if (!middleware::gameplay::external::resolve_combatant_squad_source(
                        world, actorSource, parentSource)
                    || parentSource != source) {
                    continue;
                }
                if (pair.actorSlotRow != sdk::format::kAbsentIndex) {
                    return SceneStatus::ambiguousTarget;
                }
                pair.actorSlotRow = actorIndex;
            }
        }
        if (pair.actorSlotRow == sdk::format::kAbsentIndex) {
            pair = {};
            continue;
        }
        pair.actorTarget = physical_target(catalog, pair.actorSlotRow);
        pair.sourceTarget = physical_target(catalog, catalog.squads()[pair.squadRow].slotIndex);
        ++candidate.count;
    }
    output = candidate;
    return SceneStatus::ready;
}

} // namespace sunrise::server::activity::activity_sdk_mission
