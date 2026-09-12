#include "activity_sdk_actor_sequences.h"

#include <algorithm>
#include <limits>

#include "../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../middleware/gameplay/external/actor_entity_registry.h"
#include "../../state/activity_sdk/generated_world/runtime.h"

namespace sunrise::server::activity::actor_sequences {

/** Authored references must agree before a shared actor catalog becomes slot-owned. */
bool owner(const state::activity_sdk::generated_world::GeneratedWorldView& world,
           std::uint32_t slotRow,
           Owner& output) noexcept {
    namespace sdk = state::activity_sdk;
    namespace format = sdk::format;
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    output = {};
    const auto& view = world.activity_sdk_view();
    const auto* const scenario = sdk::bound_scenario(view);
    if (view.catalog == nullptr || scenario == nullptr || view.activityClientGeneration == 0
        || slotRow >= view.catalog->slots().size()) {
        return false;
    }
    const auto& catalog = *view.catalog;
    const auto& slot = catalog.slots()[slotRow];
    if (slot.slotType != auth::kType2SlotType || slot.componentClass != auth::kType2ComponentClass
        || slot.authSchema != auth::kType2Schema || slot.senseSchema != auth::kType2SenseSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0
        || slot.objectIndex >= catalog.objects().size()
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const auto& object = catalog.objects()[slot.objectIndex];
    bool present = false;
    for (const auto& occurrence : sdk::scenario_occurrences(catalog, *scenario)) {
        present = present || occurrence.objectIndex == slot.objectIndex;
    }
    if (!present || world.snapshot() == nullptr) {
        return false;
    }
    const state::gameplay::entity_identity::ActorSourceReference source{
        object.objectKey,
        static_cast<std::uint16_t>(slot.slotIndex),
        static_cast<std::uint8_t>(slot.slotType),
        true,
        true};
    state::gameplay::entity_identity::ActorSourceReference squadSource{};
    if (!middleware::gameplay::external::resolve_combatant_squad_source(
            *world.snapshot(), source, squadSource)) {
        return false;
    }
    std::uint32_t actor = format::kAbsentIndex;
    for (const auto& squad : sdk::scenario_squads(catalog, *scenario)) {
        if (squad.slotIndex >= catalog.slots().size()
            || squad.objectIndex >= catalog.objects().size()) {
            return false;
        }
        const auto& squadSlot = catalog.slots()[squad.slotIndex];
        if (catalog.objects()[squad.objectIndex].objectKey != squadSource.key
            || squadSlot.slotIndex != squadSource.index || squadSlot.slotType != squadSource.type) {
            continue;
        }
        for (const auto& member : sdk::squad_members(catalog, squad)) {
            if ((member.flags & format::kSquadMemberActorClassExact) == 0
                || member.actorClassIndex >= catalog.actor_classes().size()
                || (actor != format::kAbsentIndex && actor != member.actorClassIndex)) {
                return false;
            }
            actor = member.actorClassIndex;
        }
    }
    if (actor == format::kAbsentIndex
        || catalog.sdk_build_sha256().size() != output.sdkBuildSha256.size()
        || catalog.payload_sha256().size() != output.sdkPayloadSha256.size()) {
        return false;
    }
    std::copy(catalog.sdk_build_sha256().begin(),
              catalog.sdk_build_sha256().end(),
              output.sdkBuildSha256.begin());
    std::copy(catalog.payload_sha256().begin(),
              catalog.payload_sha256().end(),
              output.sdkPayloadSha256.begin());
    output.activityClientGeneration = view.activityClientGeneration;
    output.sessionId = view.binding.sessionId;
    output.sessionCreatedRevision = view.binding.createdRevision;
    output.activityRow = view.activityRow;
    output.scenarioRow = view.scenarioRow;
    output.slotRow = slotRow;
    output.actorClassRow = actor;
    return true;
}

bool owner(const state::activity_sdk::BoundView& view,
           std::uint32_t slotRow,
           Owner& output) noexcept {
    state::activity_sdk::generated_world::GeneratedWorldView world{};
    output = {};
    return state::activity_sdk::generated_world::resolve(view, world)
               == state::activity_sdk::generated_world::BindStatus::ready
           && owner(world, slotRow, output);
}

} // namespace sunrise::server::activity::actor_sequences
