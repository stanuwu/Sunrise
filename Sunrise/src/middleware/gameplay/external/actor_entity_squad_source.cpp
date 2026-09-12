#include "../../bap/activity_message/scriptable_auth_body.h"
#include "../../content/packages/tables/slot_descriptor_reader.h"
#include "actor_entity_registry.h"

namespace sunrise::middleware::gameplay::external {

namespace format = state::activity_sdk::format;

/**
 * Resolves a bound combatant source through its descriptor-owned squad edge.
 * @param world Authenticated generated-world rows.
 * @param source Raw source from an accepted actor update.
 * @param output Receives the unique squad source; cleared on failure.
 * @return True when every matching edge agrees on one exact Type 1 target.
 */
bool resolve_combatant_squad_source(
    const state::build_data::scriptables::Snapshot& world,
    const state::gameplay::entity_identity::ActorSourceReference& source,
    state::gameplay::entity_identity::ActorSourceReference& output) noexcept {
    namespace catalog = state::build_data::scriptables;
    namespace auth = bap::activity_message::scriptable_auth;
    namespace tables = content::packages::tables;
    output = {};
    if (!source.known || !source.present || source.type != auth::kType2SlotType) {
        return false;
    }
    state::gameplay::entity_identity::ActorSourceReference found{};
    for (const catalog::TypedReference& edge : world.references) {
        if (edge.sourceObjectRow >= world.objects.size()
            || edge.sourceSlotRow >= world.slots.size()) {
            continue;
        }
        const auto& object = world.objects[edge.sourceObjectRow];
        const auto& slot = world.slots[edge.sourceSlotRow];
        if (!object.complete || object.registryKey != source.key
            || slot.objectRow != edge.sourceObjectRow || slot.slotType != source.type
            || slot.slotIndex != source.index || slot.firstDescriptor > world.descriptors.size()
            || slot.descriptorCount > world.descriptors.size() - slot.firstDescriptor) {
            continue;
        }
        std::size_t descriptors = 0;
        for (std::size_t index = slot.firstDescriptor;
             index < static_cast<std::size_t>(slot.firstDescriptor) + slot.descriptorCount;
             ++index) {
            const auto& descriptor = world.descriptors[index];
            descriptors += descriptor.slotRow == edge.sourceSlotRow
                                   && descriptor.configTag == edge.sourceConfigTag
                                   && descriptor.componentClass == auth::kType2ComponentClass
                                   && descriptor.senseSchema == auth::kType2SenseSchema
                                   && descriptor.authSchema == auth::kType2Schema
                                   && static_cast<std::uint64_t>(descriptor.descriptorOffset)
                                              + tables::kType2SquadReferenceOffset
                                          == edge.sourceOffset
                               ? 1U
                               : 0U;
        }
        if (descriptors == 0) {
            continue;
        }
        if (descriptors != 1 || edge.join != catalog::ReferenceJoin::exact
            || edge.targetSlotType != format::kSquadSlotType
            || edge.targetObjectRow >= world.objects.size()) {
            return false;
        }
        const auto& target = world.objects[edge.targetObjectRow];
        const std::size_t targetSlot =
            static_cast<std::size_t>(target.firstSlot) + edge.targetSlotIndex;
        if (!target.complete || target.registryKey != edge.targetKey
            || target.registryTag != object.registryTag || target.stateRow != object.stateRow
            || edge.targetSlotIndex >= target.slotCount || targetSlot >= world.slots.size()
            || world.slots[targetSlot].objectRow != edge.targetObjectRow
            || world.slots[targetSlot].slotType != format::kSquadSlotType
            || world.slots[targetSlot].slotIndex != edge.targetSlotIndex
            || world.slots[targetSlot].descriptorCount == 0) {
            return false;
        }
        const state::gameplay::entity_identity::ActorSourceReference candidate{
            edge.targetKey,
            edge.targetSlotIndex,
            static_cast<std::uint8_t>(edge.targetSlotType),
            true,
            true};
        if (found.present && found != candidate) {
            return false;
        }
        found = candidate;
    }
    output = found;
    return found.present;
}

} // namespace sunrise::middleware::gameplay::external
