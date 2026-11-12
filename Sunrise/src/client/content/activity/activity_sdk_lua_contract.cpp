#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "../../../middleware/bap/activity_message/auth_schema_catalog.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808093.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808095.h"
#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

namespace generated = middleware::content::packages::schema::generated;

/** One Lua enum entry and the class id it holds. */
struct ClassEnumEntry {
    std::string_view name;
    std::uint32_t value;
};

// Scripts use these enum names; the values are the package class ids.
constexpr std::array kCombatantLanePrimarySchema{
    ClassEnumEntry{"REF_BYTE", generated::C80807f7f::kClass},
    ClassEnumEntry{"U32", generated::C80807f7e::kClass},
    ClassEnumEntry{"REAL32", generated::C80807f7d::kClass},
    ClassEnumEntry{"REF_BYTE_BOOL", generated::C80807f7a::kClass},
    ClassEnumEntry{"EMPTY", generated::C80807f79::kClass},
    ClassEnumEntry{"U6", generated::C80807f80::kClass},
    ClassEnumEntry{"U32_BOOL", generated::C80807f78::kClass},
    ClassEnumEntry{"U32_REAL32", generated::C80807f81::kClass},
    ClassEnumEntry{"ALTERNATE_REF_BYTE", generated::C80807f77::kClass},
    ClassEnumEntry{"TRIPLE_U32_REF_MODES", generated::C80807f76::kClass},
};

// Scripts use these enum names; the values are the package class ids.
constexpr std::array kCombatantLaneSecondarySchema{
    ClassEnumEntry{"EMPTY_A", generated::C80807f8b::kClass},
    ClassEnumEntry{"EMPTY_B", generated::C80807f8a::kClass},
    ClassEnumEntry{"QUANTIZED_11", generated::C80807f89::kClass},
};

// Scripts use these enum names; the values are the package class ids.
constexpr std::array kAuthoredBehaviorFilterClass{
    ClassEnumEntry{"ACTIVITY_FLAG", generated::BehaviorConditionLeaf80804D83::kClass},
    ClassEnumEntry{"ACTOR", generated::BehaviorConditionLeaf80804D82::kClass},
    ClassEnumEntry{"AIRBORNE", generated::BehaviorConditionLeaf80804D81::kClass},
    ClassEnumEntry{"CHANNEL", generated::BehaviorChannelComparison::kClass},
    ClassEnumEntry{"HAS_PARENT", generated::BehaviorConditionLeaf80804D80::kClass},
    ClassEnumEntry{"DAMAGE_OWNER", generated::BehaviorConditionLeaf80804D7C::kClass},
    ClassEnumEntry{"DEAD", generated::BehaviorConditionLeaf80804D7B::kClass},
    ClassEnumEntry{"DISTANCE", generated::BehaviorConditionLeaf80804D7A::kClass},
    ClassEnumEntry{"FACTION", generated::BehaviorConditionStructure80804D78::kClass},
    ClassEnumEntry{"FACTION_RELATIONSHIP", generated::BehaviorConditionLeaf80804D76::kClass},
    ClassEnumEntry{"HOP_ON", generated::BehaviorConditionLeaf80804D75::kClass},
    ClassEnumEntry{"LINE_OF_SIGHT", generated::BehaviorConditionLeaf80804D74::kClass},
    ClassEnumEntry{"OBJECT_LABEL", generated::BehaviorConditionStructure80804D73::kClass},
    ClassEnumEntry{"OBJECT_TYPE", generated::BehaviorConditionLeaf80804D72::kClass},
    ClassEnumEntry{"PLAYER", generated::BehaviorConditionLeaf80804D71::kClass},
    ClassEnumEntry{"RESOURCE", generated::C80804d5f::kClass},
    ClassEnumEntry{"SAME_OBJECT", generated::C80802f95::kClass},
    ClassEnumEntry{"SCOREBOARD_STAT", generated::BehaviorConditionStructure80804D5E::kClass},
    ClassEnumEntry{"TEAM_SIDE", generated::C80804d5d::kClass},
    ClassEnumEntry{"COMPOSITE", generated::BehaviorConditionGroup::kClass},
    ClassEnumEntry{"EXTERNAL", generated::C8080930f::kClass},
};

// Scripts use these enum names; the values are the package class ids.
constexpr std::array kObjectFilterPredicateSchema{
    ClassEnumEntry{"MODE_U32_A", generated::C80809571::kClass},
    ClassEnumEntry{"MODE_U32_B", generated::C80809572::kClass},
    ClassEnumEntry{"MODE_ONLY_A", generated::C80809573::kClass},
    ClassEnumEntry{"UNREGISTERED_MODE_SLOT_REF", generated::C80809574::kClass},
    ClassEnumEntry{"MODE_U32_C", generated::C80809575::kClass},
    ClassEnumEntry{"MODE_FLAG_SLOT_REF", generated::C80809576::kClass},
    ClassEnumEntry{"MODE_SLOT_REF_A", generated::C80809577::kClass},
    ClassEnumEntry{"MODE_SLOT_REF_B", generated::C80809578::kClass},
    ClassEnumEntry{"MODE_SLOT_REF_C", generated::C80809579::kClass},
    ClassEnumEntry{"MODE_I32", generated::C8080957a::kClass},
    ClassEnumEntry{"MODE_SLOT_REF_D", generated::C8080957b::kClass},
    ClassEnumEntry{"MODE_U32_D", generated::C8080957c::kClass},
    ClassEnumEntry{"MODE_ONLY_B", generated::C8080957d::kClass},
};

/**
 * Appends one `sdk.<table> = { ... }` Lua table, then a blank line.
 * @param table Lua field name under `sdk`.
 * @param entries Enum entries in emit order.
 * @param output Receives the table text.
 * @return False when an entry does not fit the line buffer.
 */
[[nodiscard]] bool append_class_enum(std::string_view table,
                                     std::span<const ClassEnumEntry> entries,
                                     std::string& output) {
    output.append("sdk.").append(table).append(" = {\n");
    std::array<char, 96> line{};
    for (const ClassEnumEntry& entry : entries) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "    %.*s = 0x%08X,\n",
                                          static_cast<int>(entry.name.size()),
                                          entry.name.data(),
                                          entry.value);
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
            return false;
        }
        output.append(line.data(), static_cast<std::size_t>(written));
    }
    output.append("}\n\n");
    return true;
}

} // namespace

/** Emits the manifest, the mission API schema, the LuaLS declarations and the behavior module. */
bool render_contract_files(const Source& source, Bundle& output) noexcept {
    try {
        const Value manifest = object({
            {"schema", string("sunrise-activity-sdk-v11")},
            {"format_version", number(format::kVersion)},
            {"sdk_build_id", string(digest_hex(source.sdkBuildSha256, true))},
            {"sdk_payload_sha256", string(digest_hex(source.sdkPayloadSha256, false))},
            {"content_key", string(digest_hex(source.contentKeySha256, true))},
            {"logical_ir_sha256", string(digest_hex(source.logicalIrSha256, false))},
            {"counts",
             object({
                 {"activities", number(source.activities.size())},
                 {"slots", number(source.slots.size())},
                 {"states", number(source.states.size())},
                 {"squads", number(source.squads.size())},
                 {"authored_scene_resources", number(source.authoredSceneResources.size())},
                 {"authored_scene_squad_edges", number(source.authoredSceneSquadEdges.size())},
                 {"task_targets", number(source.taskTargets.size())},
                 {"dialogue_cue_texts", number(source.dialogueCueTexts.size())},
                 {"directive_elements", number(source.directiveElements.size())},
                 {"behavior_programs", number(source.behaviorPrograms.size())},
                 {"behavior_inputs", number(source.behaviorInputs.size())},
                 {"behavior_channel_writes", number(source.behaviorChannelWrites.size())},
                 {"behavior_owners", number(source.behaviorOwners.size())},
                 {"behavior_activity_bindings", number(source.behaviorActivityBindings.size())},
                 {"actor_message_schemas", number(source.actorMessageSchemas.size())},
                 {"actor_command_definitions", number(source.actorCommandDefinitions.size())},
                 {"actor_behavior_profiles", number(source.actorBehaviorProfiles.size())},
                 {"simulation_event_definitions", number(source.simulationEventDefinitions.size())},
                 {"runtime_schemas", number(source.runtimeSchemas.size())},
                 {"runtime_fields", number(source.runtimeFields.size())},
                 {"runtime_type_definitions", number(source.runtimeTypeDefinitions.size())},
                 {"sobject_rsats", number(source.sobjectRsats.size())},
                 {"sobject_rsat_descriptors", number(source.sobjectRsatDescriptors.size())},
                 {"entity_type_definitions", number(source.entityTypeDefinitions.size())},
                 {"sobject_rsat_field_bindings", number(source.sobjectRsatFieldBindings.size())},
                 {"actor_state_names", number(source.actorStateNames.size())},
                 {"actor_sequence_tables", number(source.actorSequenceTables.size())},
                 {"actor_sequence_entries", number(source.actorSequenceEntries.size())},
                 {"actor_sequence_bindings", number(source.actorSequenceBindings.size())},
                 {"authored_scene_event_keys", number(source.authoredSceneEventKeys.size())},
                 {"dialogue_cues", number(source.dialogueCues.size())},
                 {"incident_definitions", number(source.incidentDefinitions.size())},
             })},
        });
        if (!render_json(manifest, 0, output.manifestJson)) {
            return false;
        }
        output.manifestJson.push_back('\n');
        Value missionApi{};
        if (!build_mission_api_value(missionApi)
            || !render_json(missionApi, 0, output.missionApiJson)) {
            return false;
        }
        output.missionApiJson.push_back('\n');
        output.activitySdkModule =
            "-- Generated LuaLS declarations and stable SDK enums. Do not edit.\n";
        append_mission_api_luals(output.activitySdkModule);
        output.activitySdkModule +=
            R"lua(
---@class SunriseVector3
---@field x number
---@field y number
---@field z number

---@class SunriseSlot
---@field id string
---@field name string
---@field index integer
---@field type integer
---@field component_class integer
---@field sense_schema integer
---@field auth_schema integer
---@field auth_type string|nil
---@field auth_min_bits integer|nil
---@field auth_max_bits integer|nil
---@field auth_component_offset integer|nil
---@field auth_dynamic boolean|nil
---@field auth_writable boolean|nil
---@field unresourced boolean|nil

---@class SunriseDirective
---@field id string
---@field slot_row integer
---@field sensor string The directive sensor's mission.Slot value.
---@field name_hash integer
---@field element integer
---@field title string
---@field description string Empty when the element was authored without one.
---@field progress string|nil Label shown before the counter, when authored.
---@field counter boolean|nil True when the HUD shows the lane's progress values.

---@class SunriseState
---@field id string
---@field ordinal integer
---@field slice_set_index integer Authored package slice-set base.
---@field region_index integer Effective membership region for this state ordinal.
---@field hash integer
---@field value integer

---@class SunriseSquadAnchor: SunriseVector3
---@field point integer

---@class SunriseSquadMember
---@field actor_class integer|nil
---@field behavior_config integer|nil
---@field default_faction integer|nil

---@class SunriseSquad
---@field id string
---@field slot integer
---@field spawner_config integer
---@field spawn_rule_config integer
---@field members SunriseSquadMember[]
---@field anchors SunriseSquadAnchor[]

---@class SunriseScene
---@field id string
---@field slot integer
---@field config_tag integer
---@field resource_tag integer
---@field event_keys integer[]|nil

---@class SunriseTaskTarget
---@field id string
---@field task_slot integer
---@field objective_slot integer
---@field objective_bit integer
---@field config_tag integer

---@class SunriseTriggerVolume
---@field names string[]
---@field registry_key integer
---@field slot_type integer
---@field slot_index integer
---@field position SunriseVector3
---@field minimum SunriseVector3
---@field maximum SunriseVector3
---@field shape_tag integer
---@field shape_index integer
---@field active integer

---@class SunriseActorAbility
---@field slot_row integer
---@field group_hash integer
---@field request_hash integer

---@class SunriseCombatTaskGroup
---@field slot_row integer
---@field group_index integer

---@class SunriseMission
---@field name string
---@field id string
---@field tag integer
---@field states table<string, SunriseState>
---@field slots table<string, SunriseSlot>
---@field squads table<string, SunriseSquad>
---@field scenes table<string, SunriseScene>
---@field tasks table<string, SunriseTaskTarget>
---@field trigger_volumes SunriseTriggerVolume[]
---@field State table<string, integer>
---@field Slot table<string, string>
---@field Auth table<string, table<string, string>> Auth slots grouped by their native Auth type.
---@field Squad table<string, string>
---@field Scene table<string, string> Exact occurrence-bound authored scene identities.
---@field SceneInput table<string, table<string, integer>> Keys a scene waits on, by trigger name.
---@field SceneOutput table<string, table<string, integer>> Keys a scene reports, by trigger name.
---@field Task table<string, string>
---@field TaskGroup table<string, table<string, SunriseCombatTaskGroup>>
---@field ActorAbility table<string, table<string, table<string, SunriseActorAbility>>>
---@field TriggerVolume table<string, SunriseTriggerVolume>
---@field Directive table<string, SunriseDirective>
---@field PerformanceState table<string, table<string, {slot_row: integer, )lua"
            R"lua(name_hash: integer, ordinal: integer}>>
---@field DialogueCue table<string, table<string, integer>>

---@class SunriseActivity
---@field client_teleport_reset integer
---@field name string
---@field display_name string
---@field id string
---@field index integer
---@field definition_hash integer
---@field activity_root_tag integer
---@field scenario_tag integer
---@field matchmaking_config_tag integer
---@field mission SunriseMission|nil

local sdk = {}

sdk.EventKind = EventKind

sdk.SlotType = {
    SQUAD = 1,
    SEQUENCE = 5,
    CINEMATIC = 6,
    DEVICE = 23,
    TRIGGER = 31,
    TASK = 38,
    SCENE = 43,
    TRIGGER_VOLUME = 60,
}

return sdk
)lua";

        const std::size_t returnOffset = output.activitySdkModule.rfind("return sdk\n");
        if (returnOffset == std::string::npos) {
            return false;
        }
        std::string authTypes = "---@class SunriseAuthType\n"
                                "---@field name string\n"
                                "---@field slot_type integer\n"
                                "---@field schema integer\n"
                                "---@field struct_bytes integer\n"
                                "---@field min_bits integer\n"
                                "---@field max_bits integer\n"
                                "---@field component_offset integer|nil\n"
                                "---@field dynamic boolean\n"
                                "---@field writable boolean\n\n"
                                "---@type table<string, SunriseAuthType>\n"
                                "sdk.AuthType = {\n";
        std::array<char, 320> authLine{};
        for (const auto& auth : middleware::bap::activity_message::auth_schema_catalog::kTypes) {
            const int written = auth.hasContiguousMirror
                                    ? std::snprintf(authLine.data(),
                                                    authLine.size(),
                                                    "    [\"%.*s\"] = { name = \"%.*s\", "
                                                    "slot_type = %u, schema = 0x%08X, "
                                                    "struct_bytes = %u, min_bits = %u, "
                                                    "max_bits = %u, component_offset = %u, "
                                                    "dynamic = %s, writable = %s },\n",
                                                    static_cast<int>(auth.name.size()),
                                                    auth.name.data(),
                                                    static_cast<int>(auth.name.size()),
                                                    auth.name.data(),
                                                    auth.slotType,
                                                    auth.schema,
                                                    auth.structBytes,
                                                    auth.minimumBits,
                                                    auth.maximumBits,
                                                    auth.componentOffset,
                                                    auth.hasDynamicBody ? "true" : "false",
                                                    auth.writable ? "true" : "false")
                                    : std::snprintf(authLine.data(),
                                                    authLine.size(),
                                                    "    [\"%.*s\"] = { name = \"%.*s\", "
                                                    "slot_type = %u, schema = 0x%08X, "
                                                    "struct_bytes = %u, min_bits = %u, "
                                                    "max_bits = %u, component_offset = nil, "
                                                    "dynamic = %s, writable = %s },\n",
                                                    static_cast<int>(auth.name.size()),
                                                    auth.name.data(),
                                                    static_cast<int>(auth.name.size()),
                                                    auth.name.data(),
                                                    auth.slotType,
                                                    auth.schema,
                                                    auth.structBytes,
                                                    auth.minimumBits,
                                                    auth.maximumBits,
                                                    auth.hasDynamicBody ? "true" : "false",
                                                    auth.writable ? "true" : "false");
            if (written <= 0 || static_cast<std::size_t>(written) >= authLine.size()) {
                return false;
            }
            authTypes.append(authLine.data(), static_cast<std::size_t>(written));
        }
        authTypes.append(R"lua(}

---@class SunriseType2LaneClientRef
---@field registry_key integer
---@field slot_type integer
---@field slot_index integer

)lua");
        if (!append_class_enum("CombatantLanePrimarySchema", kCombatantLanePrimarySchema, authTypes)
            || !append_class_enum(
                "CombatantLaneSecondarySchema", kCombatantLaneSecondarySchema, authTypes)) {
            return false;
        }
        authTypes.append(
            "-- Authored behavior-condition classes embedded in package behavior graphs.\n");
        if (!append_class_enum(
                "AuthoredBehaviorFilterClass", kAuthoredBehaviorFilterClass, authTypes)) {
            return false;
        }
        authTypes.append(
            "-- Compact code-34 Auth predicate schemas. These are a separate runtime registry.\n");
        if (!append_class_enum(
                "ObjectFilterPredicateSchema", kObjectFilterPredicateSchema, authTypes)) {
            return false;
        }
        authTypes.append(R"lua(---@class SunriseObjectFilterModeOnly
---@field mode integer

---@class SunriseObjectFilterModeU32: SunriseObjectFilterModeOnly
---@field value integer

---@class SunriseObjectFilterModeSlotRef: SunriseObjectFilterModeOnly
---@field reference SunriseType2LaneClientRef

---@class SunriseObjectFilterModeFlagSlotRef: SunriseObjectFilterModeSlotRef
---@field flag boolean

)lua");
        append_actor_sequence_contract(source, authTypes);
        output.activitySdkModule.insert(returnOffset, authTypes);

        output.behaviorModule =
            "-- Generated compiled behavior declarations. Do not edit.\n"
            "local behavior = {}\n\n"
            "behavior.InputSelector = { TARGET_OBJECT = 0, CONTEXT_PRIMARY_OBJECT = 1, "
            "CONTEXT_SECONDARY_OBJECT = 2 }\n"
            "behavior.InputRole = { ACTION = 1, CONDITION_LEFT = 2, CONDITION_RIGHT = 3 }\n\n"
            "behavior.Program = {\n";
        std::array<char, 320> line{};
        for (const format::BehaviorProgram& row : source.behaviorPrograms) {
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "    ROOT_%08X = { tag = 0x%08X, first_input = %u, input_count = %u, "
                "first_write = %u, write_count = %u, node_count = %u, expression_count = %u },\n",
                row.rootTag,
                row.rootTag,
                row.inputs.first,
                row.inputs.count,
                row.channelWrites.first,
                row.channelWrites.count,
                row.nodeCount,
                row.expressionCount);
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nbehavior.Channel = {\n");
        std::vector<std::uint32_t> behaviorChannels{};
        behaviorChannels.reserve(source.behaviorInputs.size()
                                 + source.behaviorChannelWrites.size());
        for (const format::BehaviorInput& row : source.behaviorInputs) {
            behaviorChannels.push_back(row.channelHash);
        }
        for (const format::BehaviorChannelWrite& row : source.behaviorChannelWrites) {
            behaviorChannels.push_back(row.channelHash);
        }
        std::sort(behaviorChannels.begin(), behaviorChannels.end());
        behaviorChannels.erase(std::unique(behaviorChannels.begin(), behaviorChannels.end()),
                               behaviorChannels.end());
        for (const std::uint32_t channel : behaviorChannels) {
            const int written = std::snprintf(
                line.data(), line.size(), "    CHANNEL_%08X = 0x%08X,\n", channel, channel);
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nbehavior.Input = {\n");
        for (const format::BehaviorInput& row : source.behaviorInputs) {
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "    { program = %u, node_offset = 0x%X, expression_offset = 0x%X, "
                              "channel = behavior.Channel.CHANNEL_%08X, input_or_mode = \"%llu\", "
                              "native_override = %d, "
                              "active_field = %u, selector = %u, role = %u },\n",
                              row.programIndex,
                              row.nodeOffset,
                              row.expressionOffset,
                              row.channelHash,
                              static_cast<unsigned long long>(row.inputOrMode),
                              row.nativeOverride,
                              row.activeField,
                              static_cast<unsigned int>(row.selector),
                              static_cast<unsigned int>(row.role));
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nbehavior.ChannelWrite = {\n");
        for (const format::BehaviorChannelWrite& row : source.behaviorChannelWrites) {
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "    { program = %u, node_offset = 0x%X, "
                                              "channel = behavior.Channel.CHANNEL_%08X },\n",
                                              row.programIndex,
                                              row.nodeOffset,
                                              row.channelHash);
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nbehavior.SubmissionKind = { ACTIVE_NATIVE = 0, PASSIVE "
                                     "= 1, UNRESOLVED = 2 }\n\nbehavior.Owner = {\n");
        for (const format::BehaviorOwner& row : source.behaviorOwners) {
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "    { program = %u, actor_class = %u, config = 0x%08X, "
                              "config_offset = 0x%X, build = %u, descriptor = %u, subtype = "
                              "0x%08X, kind = %u },\n",
                              row.programIndex,
                              row.actorClassIndex,
                              row.configTag,
                              row.configFieldOffset,
                              row.buildOrdinal,
                              row.descriptorOrdinal,
                              row.submitterSubtype,
                              static_cast<unsigned int>(row.submissionKind));
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nbehavior.ActivityBinding = {\n");
        for (const format::BehaviorActivityBinding& row : source.behaviorActivityBindings) {
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "    { owner = %u, squad = %u, member = %u, scenario "
                                              "= %u, occurrence = %u, state = %u, object = %u },\n",
                                              row.ownerIndex,
                                              row.squadIndex,
                                              row.squadMemberIndex,
                                              row.scenarioIndex,
                                              row.occurrenceIndex,
                                              row.stateIndex,
                                              row.objectIndex);
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                return false;
            }
            output.behaviorModule.append(line.data(), static_cast<std::size_t>(written));
        }
        output.behaviorModule.append("}\n\nreturn behavior\n");
        return render_incident_module(source, output.incidentModule);
    } catch (...) {
        output.manifestJson.clear();
        output.missionApiJson.clear();
        output.activitySdkModule.clear();
        output.behaviorModule.clear();
        output.incidentModule.clear();
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
