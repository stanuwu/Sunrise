#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mission_script_world_sdk_internal.h"
#include "mission_script_world_sdk_values.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {

namespace generated_world = state::activity_sdk::generated_world;
namespace catalog = state::build_data::scriptables;

} // namespace

/**
 * Reads one named field from one generated-world row.
 * @param localRow One-based row inside the collection named by kind.
 * @param key Row field name; an unknown key yields an absent value rather than a failure.
 * @return False only when the generation, row, or collection is invalid.
 */
bool world_sdk_internal::resolve_world_field(const void* context,
                                             const lua_vm::WorldGenerationIdentity& generation,
                                             lua_vm::WorldCollectionKind kind,
                                             std::uint32_t localRow,
                                             std::string_view key,
                                             lua_vm::WorldFieldDefinition& output) noexcept {
    output = {};
    const generated_world::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || localRow == 0
        || localRow > world_sdk_internal::world_count(context, generation, kind)) {
        return false;
    }
    const catalog::Snapshot& snapshot = *world->snapshot();
    const std::size_t index = localRow - 1U;
    if (key == "row") {
        return field_u64(output, localRow);
    }
#define WORLD_FIELD_U32(lua_key, member)                                                           \
    if (key == (lua_key)) {                                                                        \
        return field_u64(output, member);                                                          \
    }
#define WORLD_FIELD_U64(lua_key, member)                                                           \
    if (key == (lua_key)) {                                                                        \
        return field_u64(output, member, lua_vm::WorldFieldKind::unsignedDecimalString);           \
    }
#define WORLD_FIELD_I32(lua_key, member)                                                           \
    if (key == (lua_key)) {                                                                        \
        return field_i64(output, member);                                                          \
    }
#define WORLD_FIELD_I64(lua_key, member)                                                           \
    if (key == (lua_key)) {                                                                        \
        return field_i64(output, member, lua_vm::WorldFieldKind::signedDecimalString);             \
    }
#define WORLD_FIELD_BOOL(lua_key, member)                                                          \
    if (key == (lua_key)) {                                                                        \
        return field_bool(output, member);                                                         \
    }
#define WORLD_FIELD_ROW(lua_key, member)                                                           \
    if (key == (lua_key)) {                                                                        \
        return field_row(output, member);                                                          \
    }
#define WORLD_FIELD_FIRST(lua_key, first, count)                                                   \
    if (key == (lua_key)) {                                                                        \
        return field_first_row(output, first, count);                                              \
    }
#define WORLD_FIELD_VECTOR(lua_key, member)                                                        \
    if (key == (lua_key)) {                                                                        \
        return field_vector(output, member);                                                       \
    }
#define WORLD_FIELD_BYTES(lua_key, member)                                                         \
    if (key == (lua_key)) {                                                                        \
        return field_bytes(output, member);                                                        \
    }
#define WORLD_FIELD_NAME(prefix, member, is_tag)                                                   \
    if (selected_name_field(snapshot, member, is_tag, key, prefix, output)) {                      \
        return true;                                                                               \
    }

    switch (kind) {
    case lua_vm::WorldCollectionKind::bubbles: {
        const catalog::Bubble& row = snapshot.bubbles[index];
        WORLD_FIELD_U32("name_hash", row.nameHash)
        WORLD_FIELD_FIRST("first_state_row", row.firstState, row.stateCount)
        WORLD_FIELD_U32("state_count", row.stateCount)
        WORLD_FIELD_ROW("name_row", row.nameRow)
        WORLD_FIELD_U32("index", row.index)
        WORLD_FIELD_BOOL("is_public", row.isPublic)
        WORLD_FIELD_NAME("selected", row.nameRow, false)
        break;
    }
    case lua_vm::WorldCollectionKind::states: {
        const catalog::State& row = snapshot.states[index];
        WORLD_FIELD_U32("state_hash", row.stateHash)
        WORLD_FIELD_U32("raw_u32_at_12", row.rawU32At12)
        WORLD_FIELD_U32("entry_tag", row.entryTag)
        WORLD_FIELD_U32("registry_tag", row.registryTag)
        WORLD_FIELD_U32("slice_set_index", row.sliceSetIndex)
        WORLD_FIELD_U32("map_bubble_index", row.mapBubbleIndex)
        WORLD_FIELD_ROW("bubble_row", row.bubbleRow)
        WORLD_FIELD_ROW("name_row", row.nameRow)
        WORLD_FIELD_ROW("entry_name_row", row.entryNameRow)
        WORLD_FIELD_ROW("registry_name_row", row.registryNameRow)
        WORLD_FIELD_U32("index", row.index)
        WORLD_FIELD_BOOL("enabled", row.enabled)
        WORLD_FIELD_BOOL("resolved", row.resolved)
        WORLD_FIELD_NAME("selected", row.nameRow, false)
        WORLD_FIELD_NAME("entry", row.entryNameRow, true)
        WORLD_FIELD_NAME("registry", row.registryNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::objects: {
        const catalog::Object& row = snapshot.objects[index];
        WORLD_FIELD_ROW("bubble_row", row.bubbleRow)
        WORLD_FIELD_ROW("state_row", row.stateRow)
        WORLD_FIELD_U32("registry_tag", row.registryTag)
        WORLD_FIELD_U32("object_tag", row.objectTag)
        WORLD_FIELD_U32("registry_key", row.registryKey)
        WORLD_FIELD_FIRST("first_slot_row", row.firstSlot, row.slotCount)
        WORLD_FIELD_U32("slot_count", row.slotCount)
        WORLD_FIELD_U32("config_count", row.configCount)
        WORLD_FIELD_U32("placed_subblock_count", row.placedSubblockCount)
        WORLD_FIELD_U32("placed_leaf_count", row.placedLeafCount)
        WORLD_FIELD_U32("placed_hop_count", row.placedHopCount)
        WORLD_FIELD_U32("bare_target_count", row.bareTargetCount)
        WORLD_FIELD_U32("replicated_placement_count", row.replicatedPlacementCount)
        WORLD_FIELD_U32("object_index", row.objectIndex)
        WORLD_FIELD_ROW("registry_name_row", row.registryNameRow)
        WORLD_FIELD_ROW("object_name_row", row.objectNameRow)
        WORLD_FIELD_U32("registry_descriptor", row.registryDescriptor)

        if (key == "safety") {
            const char* const text = safety_text(row.safety);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("safety_code", static_cast<std::uint32_t>(row.safety))
        WORLD_FIELD_BOOL("complete", row.complete)
        WORLD_FIELD_NAME("registry", row.registryNameRow, true)
        WORLD_FIELD_NAME("object", row.objectNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::slots: {
        const catalog::Slot& row = snapshot.slots[index];
        WORLD_FIELD_ROW("object_row", row.objectRow)
        WORLD_FIELD_U32("name_hash", row.nameHash)
        WORLD_FIELD_ROW("name_row", row.nameRow)
        WORLD_FIELD_FIRST("first_descriptor_row", row.firstDescriptor, row.descriptorCount)
        WORLD_FIELD_U32("descriptor_count", row.descriptorCount)
        WORLD_FIELD_U32("slot_index", row.slotIndex)
        WORLD_FIELD_U32("slot_type", row.slotType)
        WORLD_FIELD_NAME("selected", row.nameRow, false)
        break;
    }
    case lua_vm::WorldCollectionKind::descriptors: {
        const catalog::Descriptor& row = snapshot.descriptors[index];
        WORLD_FIELD_ROW("slot_row", row.slotRow)
        WORLD_FIELD_U32("config_tag", row.configTag)
        WORLD_FIELD_U32("component_class", row.componentClass)
        WORLD_FIELD_U32("sense_schema", row.senseSchema)
        WORLD_FIELD_U32("auth_schema", row.authSchema)
        WORLD_FIELD_U32("descriptor_offset", row.descriptorOffset)
        WORLD_FIELD_U32("bubble_index", row.bubbleIndex)
        WORLD_FIELD_ROW("config_name_row", row.configNameRow)
        WORLD_FIELD_U64("placement_identifier", row.placementIdentifier)
        WORLD_FIELD_ROW("placement_link_row", row.placementLinkRow)
        WORLD_FIELD_ROW("embedded_placement_link_row", row.embeddedPlacementLinkRow)
        WORLD_FIELD_BOOL("placement_identifier_read", row.placementIdentifierRead)
        WORLD_FIELD_NAME("config", row.configNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::embeddedPlacementLinks: {
        const catalog::EmbeddedPlacementLink& row = snapshot.embeddedPlacementLinks[index];
        WORLD_FIELD_ROW("descriptor_row", row.descriptorRow)
        WORLD_FIELD_ROW("slot_row", row.slotRow)
        WORLD_FIELD_ROW("object_row", row.objectRow)
        WORLD_FIELD_FIRST("first_candidate_row", row.firstCandidate, row.candidateCount)
        WORLD_FIELD_U32("candidate_count", row.candidateCount)
        WORLD_FIELD_U64("declared_placement_count", row.declaredPlacementCount)
        WORLD_FIELD_U64("array_data_offset", row.arrayDataOffset)
        WORLD_FIELD_BOOL("complete", row.complete)
        break;
    }
    case lua_vm::WorldCollectionKind::typedReferences: {
        const catalog::TypedReference& row = snapshot.references[index];
        WORLD_FIELD_ROW("source_object_row", row.sourceObjectRow)
        WORLD_FIELD_ROW("source_slot_row", row.sourceSlotRow)
        WORLD_FIELD_U32("source_config_tag", row.sourceConfigTag)
        WORLD_FIELD_U32("source_offset", row.sourceOffset)
        WORLD_FIELD_ROW("target_object_row", row.targetObjectRow)
        WORLD_FIELD_U32("target_key", row.targetKey)
        WORLD_FIELD_U32("target_slot_index", row.targetSlotIndex)
        WORLD_FIELD_U32("target_slot_type", row.targetSlotType)
        WORLD_FIELD_ROW("source_config_name_row", row.sourceConfigNameRow)

        if (key == "join") {
            const char* const text = reference_join_text(row.join);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("join_code", static_cast<std::uint32_t>(row.join))
        WORLD_FIELD_NAME("source_config", row.sourceConfigNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::containerPlacementLists: {
        const catalog::ContainerPlacementList& row = snapshot.containerPlacementLists[index];
        WORLD_FIELD_U32("object_list_tag", row.objectListTag)
        WORLD_FIELD_U32("resource_tag", row.resourceTag)
        WORLD_FIELD_U32("resource_class", row.resourceClass)
        WORLD_FIELD_ROW("object_list_name_row", row.objectListNameRow)
        WORLD_FIELD_ROW("resource_name_row", row.resourceNameRow)
        WORLD_FIELD_BOOL("resource_field_read", row.resourceFieldRead)
        WORLD_FIELD_BOOL("resource_resolved", row.resourceResolved)
        WORLD_FIELD_BOOL("complete", row.complete)
        WORLD_FIELD_NAME("object_list", row.objectListNameRow, true)
        WORLD_FIELD_NAME("resource", row.resourceNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::containerPlacementOwners: {
        const catalog::ContainerPlacementOwner& row = snapshot.containerPlacementOwners[index];
        WORLD_FIELD_ROW("list_row", row.listRow)
        WORLD_FIELD_U32("container_tag", row.containerTag)
        WORLD_FIELD_U32("member_index", row.memberIndex)
        WORLD_FIELD_ROW("container_name_row", row.containerNameRow)
        WORLD_FIELD_U64("scenario_bubble_mask", row.scenarioBubbleMask)
        WORLD_FIELD_BYTES("map_bubble_mask", row.mapBubbleMask)

        if (key == "context") {
            const char* const text = spatial_context_text(row.context);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("context_code", static_cast<std::uint32_t>(row.context))
        WORLD_FIELD_NAME("container", row.containerNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::containerPlacementConfigs: {
        const catalog::ContainerPlacementConfig& row = snapshot.containerPlacementConfigs[index];
        WORLD_FIELD_ROW("placement_row", row.placementRow)
        WORLD_FIELD_U32("config_tag", row.configTag)
        WORLD_FIELD_ROW("config_name_row", row.configNameRow)
        WORLD_FIELD_FIRST("first_component_row", row.firstComponent, row.componentCount)
        WORLD_FIELD_U32("component_count", row.componentCount)
        WORLD_FIELD_U32("build_ordinal", row.buildOrdinal)
        WORLD_FIELD_U32("second_word", row.secondWord)
        WORLD_FIELD_U32("third_word", row.thirdWord)
        WORLD_FIELD_BOOL("complete", row.complete)
        WORLD_FIELD_NAME("config", row.configNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::containerPlacementComponents: {
        const catalog::ContainerPlacementComponent& row =
            snapshot.containerPlacementComponents[index];
        WORLD_FIELD_ROW("config_row", row.configRow)
        WORLD_FIELD_U32("component_class", row.componentClass)
        WORLD_FIELD_U32("first_word", row.firstWord)
        WORLD_FIELD_U32("second_word", row.secondWord)
        WORLD_FIELD_U32("fourth_word", row.fourthWord)
        WORLD_FIELD_U32("ordinal", row.ordinal)
        break;
    }
    case lua_vm::WorldCollectionKind::type23PlacementLinks: {
        const catalog::Type23PlacementLink& row = snapshot.type23PlacementLinks[index];
        WORLD_FIELD_ROW("descriptor_row", row.descriptorRow)
        WORLD_FIELD_ROW("slot_row", row.slotRow)
        WORLD_FIELD_FIRST("first_candidate_row", row.firstCandidate, row.candidateCount)
        WORLD_FIELD_U32("candidate_count", row.candidateCount)
        WORLD_FIELD_U32("identity_match_count", row.identityMatchCount)
        WORLD_FIELD_U32("active_candidate_count", row.activeCandidateCount)
        WORLD_FIELD_ROW("resolved_candidate_row", row.resolvedCandidate)
        WORLD_FIELD_U64("placement_identifier", row.placementIdentifier)

        if (key == "join") {
            const char* const text = reference_join_text(row.join);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("join_code", static_cast<std::uint32_t>(row.join))
        WORLD_FIELD_BOOL("complete", row.complete)
        break;
    }
    case lua_vm::WorldCollectionKind::type23PlacementCandidates: {
        const catalog::Type23PlacementCandidate& row = snapshot.type23PlacementCandidates[index];
        WORLD_FIELD_ROW("link_row", row.linkRow)
        WORLD_FIELD_ROW("placement_row", row.placementRow)
        WORLD_FIELD_ROW("owner_row", row.ownerRow)
        WORLD_FIELD_U32("applicable_owner_count", row.applicableOwnerCount)
        break;
    }
    case lua_vm::WorldCollectionKind::staticSpatialTables: {
        const catalog::StaticSpatialTable& row = snapshot.staticSpatialTables[index];
        WORLD_FIELD_U32("table_tag", row.tableTag)
        WORLD_FIELD_U32("bounds_tag", row.boundsTag)
        WORLD_FIELD_FIRST("first_instance_row", row.firstInstance, row.instanceCount)
        WORLD_FIELD_U32("instance_count", row.instanceCount)
        WORLD_FIELD_ROW("table_name_row", row.tableNameRow)
        WORLD_FIELD_ROW("bounds_name_row", row.boundsNameRow)
        WORLD_FIELD_BOOL("complete", row.complete)
        WORLD_FIELD_NAME("table", row.tableNameRow, true)
        WORLD_FIELD_NAME("bounds", row.boundsNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::staticSpatialOwners: {
        const catalog::StaticSpatialOwner& row = snapshot.staticSpatialOwners[index];
        WORLD_FIELD_ROW("table_row", row.tableRow)
        WORLD_FIELD_ROW("placement_row", row.placementRow)
        WORLD_FIELD_U32("container_tag", row.containerTag)
        WORLD_FIELD_U32("object_list_tag", row.objectListTag)
        WORLD_FIELD_U32("parent_tag", row.parentTag)
        WORLD_FIELD_U32("object_list_entry", row.objectListEntry)
        WORLD_FIELD_ROW("container_name_row", row.containerNameRow)
        WORLD_FIELD_ROW("object_list_name_row", row.objectListNameRow)
        WORLD_FIELD_ROW("parent_name_row", row.parentNameRow)
        WORLD_FIELD_U64("scenario_bubble_mask", row.scenarioBubbleMask)
        WORLD_FIELD_BYTES("map_bubble_mask", row.mapBubbleMask)

        if (key == "context") {
            const char* const text = spatial_context_text(row.context);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("context_code", static_cast<std::uint32_t>(row.context))
        WORLD_FIELD_NAME("container", row.containerNameRow, true)
        WORLD_FIELD_NAME("object_list", row.objectListNameRow, true)
        WORLD_FIELD_NAME("parent", row.parentNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::triggerVolumeTables: {
        const catalog::TriggerVolumeTable& row = snapshot.triggerVolumeTables[index];
        WORLD_FIELD_U32("config_tag", row.configTag)
        WORLD_FIELD_ROW("config_name_row", row.configNameRow)
        WORLD_FIELD_FIRST("first_instance_row", row.firstInstance, row.instanceCount)
        WORLD_FIELD_U32("instance_count", row.instanceCount)
        WORLD_FIELD_U32("identity_match_count", row.identityMatchCount)
        WORLD_FIELD_U32("registry_key", row.registryKey)
        WORLD_FIELD_U32("component_ordinal", row.componentOrdinal)
        WORLD_FIELD_U32("slot_index", row.slotIndex)
        WORLD_FIELD_U32("slot_type", row.slotType)
        WORLD_FIELD_BOOL("complete", row.complete)
        WORLD_FIELD_NAME("config", row.configNameRow, true)
        break;
    }
    case lua_vm::WorldCollectionKind::triggerVolumeOwners: {
        const catalog::TriggerVolumeOwner& row = snapshot.triggerVolumeOwners[index];
        WORLD_FIELD_ROW("table_row", row.tableRow)
        WORLD_FIELD_ROW("object_row", row.objectRow)
        WORLD_FIELD_ROW("slot_row", row.slotRow)
        WORLD_FIELD_U32("slot_match_count", row.slotMatchCount)
        WORLD_FIELD_FIRST(
            "first_incoming_reference_row", row.firstIncomingReference, row.incomingReferenceCount)
        WORLD_FIELD_U32("incoming_reference_count", row.incomingReferenceCount)
        WORLD_FIELD_U32("incoming_reference_match_count", row.incomingReferenceMatchCount)

        if (key == "slot_join") {
            const char* const text = reference_join_text(row.slotJoin);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("slot_join_code", static_cast<std::uint32_t>(row.slotJoin))
        break;
    }
    case lua_vm::WorldCollectionKind::triggerVolumeIncomingReferences: {
        const catalog::TriggerVolumeIncomingReference& row =
            snapshot.triggerVolumeIncomingReferences[index];
        WORLD_FIELD_ROW("owner_row", row.ownerRow)
        WORLD_FIELD_ROW("reference_row", row.referenceRow)
        WORLD_FIELD_ROW("source_object_row", row.sourceObjectRow)
        WORLD_FIELD_ROW("source_slot_row", row.sourceSlotRow)
        break;
    }
    case lua_vm::WorldCollectionKind::triggerVolumeVertices: {
        const catalog::TriggerVolumeVertex& row = snapshot.triggerVolumeVertices[index];
        WORLD_FIELD_VECTOR("value", row.value)
        break;
    }
    case lua_vm::WorldCollectionKind::triggerVolumeTriangles: {
        const catalog::TriggerVolumeTriangle& row = snapshot.triggerVolumeTriangles[index];
        WORLD_FIELD_U32("a", row.indices[0])
        WORLD_FIELD_U32("b", row.indices[1])
        WORLD_FIELD_U32("c", row.indices[2])
        break;
    }
    case lua_vm::WorldCollectionKind::names: {
        const catalog::Name& row = snapshot.names[index];
        WORLD_FIELD_U32("hash", row.hash)
        WORLD_FIELD_FIRST("first_candidate_row", row.firstCandidate, row.candidateCount)
        WORLD_FIELD_U32("candidate_count", row.candidateCount)
        WORLD_FIELD_ROW("selected_candidate_row", row.selectedCandidate)

        if (key == "provenance") {
            const char* const text = provenance_text(static_cast<std::uint32_t>(row.provenance));
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("provenance_code", static_cast<std::uint32_t>(row.provenance))
        WORLD_FIELD_BOOL("strongest_tier_overflow", row.strongestTierOverflow)
        WORLD_FIELD_NAME("selected", static_cast<std::uint32_t>(index), false)
        break;
    }
    case lua_vm::WorldCollectionKind::tagNames: {
        const catalog::TagName& row = snapshot.tagNames[index];
        WORLD_FIELD_U32("tag", row.tag)
        WORLD_FIELD_U32("class_id", row.classId)
        WORLD_FIELD_FIRST("first_candidate_row", row.firstCandidate, row.candidateCount)
        WORLD_FIELD_U32("candidate_count", row.candidateCount)
        WORLD_FIELD_ROW("selected_candidate_row", row.selectedCandidate)

        if (key == "provenance") {
            const char* const text = provenance_text(static_cast<std::uint32_t>(row.provenance));
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("provenance_code", static_cast<std::uint32_t>(row.provenance))
        WORLD_FIELD_NAME("selected", static_cast<std::uint32_t>(index), true)
        break;
    }
    case lua_vm::WorldCollectionKind::nameCandidates: {
        const catalog::NameCandidate& row = snapshot.nameCandidates[index];
        if (key == "value") {
            if (row.length > row.value.size()) {
                return false;
            }
            return field_string(output, {row.value.data(), row.length});
        }
        WORLD_FIELD_U32("source_tag", row.sourceTag)
        WORLD_FIELD_U32("source_class_id", row.sourceClassId)
        WORLD_FIELD_U32("length", row.length)

        if (key == "provenance") {
            const char* const text = provenance_text(static_cast<std::uint32_t>(row.provenance));
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("provenance_code", static_cast<std::uint32_t>(row.provenance))
        break;
    }
    case lua_vm::WorldCollectionKind::inlineNameCandidates: {
        const catalog::InlineNameCandidate& row = snapshot.inlineNameCandidates[index];
        WORLD_FIELD_U32("hash", row.hash)
        WORLD_FIELD_U32("first_byte", row.firstByte)
        WORLD_FIELD_U32("byte_count", row.byteCount)

        if (key == "value") {
            if (row.firstByte > snapshot.inlineNameBytes.size()
                || row.byteCount > snapshot.inlineNameBytes.size() - row.firstByte) {
                return false;
            }
            if (row.byteCount == 0) {
                return field_string(output, {});
            }
            const auto* const bytes = snapshot.inlineNameBytes.data() + row.firstByte;
            return field_string(output, {reinterpret_cast<const char*>(bytes), row.byteCount});
        }
        break;
    }
    case lua_vm::WorldCollectionKind::authoredSquadConfigContexts: {
        const catalog::AuthoredSquadConfigContext& row =
            snapshot.authoredSquadConfigContexts[index];
        WORLD_FIELD_ROW("global_row", row.globalRow)
        WORLD_FIELD_ROW("scenario_row", row.scenarioIndex)
        WORLD_FIELD_U32("config_tag", row.configTag)
        WORLD_FIELD_ROW("occurrence_row", row.occurrenceIndex)
        WORLD_FIELD_ROW("object_row", row.objectIndex)
        WORLD_FIELD_U32("path_ordinal", row.pathOrdinal)
        WORLD_FIELD_I32("declared_bubble_index", row.declaredBubbleIndex)
        WORLD_FIELD_ROW("spawner_row", row.spawnerRow)
        WORLD_FIELD_ROW("rule_row", row.ruleRow)
        WORLD_FIELD_BOOL("complete", row.complete)
        break;
    }
    case lua_vm::WorldCollectionKind::authoredSquadPlacementContexts: {
        const catalog::AuthoredSquadPlacementContext& row =
            snapshot.authoredSquadPlacementContexts[index];
        WORLD_FIELD_ROW("global_row", row.globalRow)
        WORLD_FIELD_ROW("scenario_row", row.scenarioIndex)
        WORLD_FIELD_ROW("occurrence_row", row.occurrenceIndex)
        WORLD_FIELD_U32("object_list_tag", row.objectListTag)
        WORLD_FIELD_U32("placement_ordinal", row.placementOrdinal)
        WORLD_FIELD_U64("placed_entry_identity", row.placedEntryIdentity)
        WORLD_FIELD_U32("position_bits_x", row.positionBits[0])
        WORLD_FIELD_U32("position_bits_y", row.positionBits[1])
        WORLD_FIELD_U32("position_bits_z", row.positionBits[2])
        WORLD_FIELD_ROW("object_row", row.objectIndex)
        WORLD_FIELD_U32("path_ordinal", row.pathOrdinal)
        WORLD_FIELD_I32("declared_bubble_index", row.declaredBubbleIndex)
        // A tag is not a row index, so it is published raw and absent when unresolved.
        if (key == "actor_definition_tag") {
            if (row.actorDefinitionTag == catalog::kNoRow) {
                break;
            }
            return field_u64(output, row.actorDefinitionTag);
        }
        WORLD_FIELD_U64("source_offset", row.sourceOffset)
        WORLD_FIELD_U32("rotation_bits_x", row.quaternionBits[0])
        WORLD_FIELD_U32("rotation_bits_y", row.quaternionBits[1])
        WORLD_FIELD_U32("rotation_bits_z", row.quaternionBits[2])
        WORLD_FIELD_U32("rotation_bits_w", row.quaternionBits[3])
        WORLD_FIELD_U32("uniform_scale_bits", row.uniformScaleBits)
        WORLD_FIELD_U32("name_hash", row.nameHash)
        WORLD_FIELD_U32("placement_flags_raw", row.placementFlagsRaw)
        WORLD_FIELD_I64("auxiliary_relative", row.auxiliaryRelative)
        WORLD_FIELD_BOOL("complete", row.complete)
        break;
    }
    case lua_vm::WorldCollectionKind::authoredSquadPointContexts: {
        const catalog::AuthoredSquadPointContext& row = snapshot.authoredSquadPointContexts[index];
        WORLD_FIELD_ROW("global_row", row.globalRow)
        WORLD_FIELD_ROW("scenario_row", row.scenarioIndex)
        WORLD_FIELD_ROW("point_row", row.pointRow)
        WORLD_FIELD_ROW("global_config_context_row", row.globalConfigContextRow)
        WORLD_FIELD_ROW("config_context_row", row.configContextRow)
        WORLD_FIELD_ROW("global_first_match_row", row.globalFirstMatch)
        WORLD_FIELD_FIRST("first_match_row", row.firstMatch, row.matchCount)
        WORLD_FIELD_U32("match_count", row.matchCount)

        if (key == "status") {
            const char* const text = point_context_status_text(row.status);
            return field_string(output, text == nullptr ? std::string_view{} : text, true);
        }
        WORLD_FIELD_U32("status_code", static_cast<std::uint32_t>(row.status))
        break;
    }
    case lua_vm::WorldCollectionKind::authoredSquadPointPlacementMatches: {
        const catalog::AuthoredSquadPointPlacementMatch& row =
            snapshot.authoredSquadPointPlacementMatches[index];
        WORLD_FIELD_ROW("global_row", row.globalRow)
        WORLD_FIELD_ROW("scenario_row", row.scenarioIndex)
        WORLD_FIELD_ROW("global_point_context_row", row.globalPointContextRow)
        WORLD_FIELD_ROW("point_context_row", row.pointContextRow)
        WORLD_FIELD_ROW("point_row", row.pointRow)
        WORLD_FIELD_ROW("global_config_context_row", row.globalConfigContextRow)
        WORLD_FIELD_ROW("config_context_row", row.configContextRow)
        WORLD_FIELD_ROW("global_placement_context_row", row.globalPlacementContextRow)
        WORLD_FIELD_ROW("placement_context_row", row.placementContextRow)
        WORLD_FIELD_U64("placed_entry_identity", row.placedEntryIdentity)
        WORLD_FIELD_BOOL("same_occurrence", row.sameOccurrence)
        break;
    }
    case lua_vm::WorldCollectionKind::authoredSquadEdgeContexts: {
        const catalog::AuthoredSquadEdgeContext& row = snapshot.authoredSquadEdgeContexts[index];
        WORLD_FIELD_ROW("global_row", row.globalRow)
        WORLD_FIELD_ROW("scenario_row", row.scenarioIndex)
        WORLD_FIELD_ROW("edge_row", row.edgeRow)
        break;
    }
    case lua_vm::WorldCollectionKind::squadAnchors:
    case lua_vm::WorldCollectionKind::authoredPlacements:
    case lua_vm::WorldCollectionKind::embeddedPlacements:
    case lua_vm::WorldCollectionKind::containerPlacements:
    case lua_vm::WorldCollectionKind::staticSpatialInstances:
    case lua_vm::WorldCollectionKind::triggerVolumes:
        break;
    }

#undef WORLD_FIELD_NAME
#undef WORLD_FIELD_BYTES
#undef WORLD_FIELD_VECTOR
#undef WORLD_FIELD_FIRST
#undef WORLD_FIELD_ROW
#undef WORLD_FIELD_BOOL
#undef WORLD_FIELD_I64
#undef WORLD_FIELD_I32
#undef WORLD_FIELD_U64
#undef WORLD_FIELD_U32
    output = {};
    output.kind = lua_vm::WorldFieldKind::absent;
    return true;
}

} // namespace sunrise::server::activity::mission::sdk_bridge
