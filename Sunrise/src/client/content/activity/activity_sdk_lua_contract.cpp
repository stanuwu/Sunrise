#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "../../../middleware/bap/activity_message/auth_schema_catalog.h"
#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

/** Emits only generated activity identity and panel-data declarations. */
bool render_contract_files(const Source& source, Bundle& output) noexcept {
    try {
        const Value manifest = object({
            {"schema", string("sunrise-activity-sdk-v8")},
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
             })},
        });
        if (!render_json(manifest, 0, output.manifestJson)) {
            return false;
        }
        output.manifestJson.push_back('\n');
        output.activitySdkModule =
            R"lua(-- Generated LuaLS declarations and stable SDK enums. Do not edit.
---@enum SunriseEventKind
local EventKind = {
    SENSOR_SENSE_UPDATED = 0,
    CLIENT_STATE_CHANGED = 1,
    CLIENT_MESSAGE_RECEIVED = 2,
    AUTH_STATE_COMMITTED = 3,
    AUTH_STATE_TRANSPORT_STAGED = 4,
    AUTH_STATE_CANCELED = 5,
    INCIDENT_RECEIVED = 6,
    INCIDENT_QUEUED = 7,
    INCIDENT_TRANSPORT_STAGED = 8,
    INCIDENT_CANCELED = 9,
    INCIDENT_REFUSED = 10,
    SCRIPTABLE_OVERRIDE_COMMITTED = 11,
    SCRIPTABLE_OVERRIDE_TRANSPORT_STAGED = 12,
    SCRIPTABLE_OVERRIDE_CANCELED = 13,
    OPERATOR_REFUSED = 14,
    TIMER_ELAPSED = 15,
    EFFECT_RESULT = 16,
    PHASE_ENTERED = 17,
    TRIGGER_ENTERED = 18,
    TRIGGER_EXITED = 19,
    SQUAD_STATE = 20,
    ENTITY_SPAWNED = 21,
    ENTITY_DIED = 22,
    SCENE_FINISHED = 23,
    OBJECTIVE_PROGRESS = 24,
    ENTITY_SLOTS_REQUESTED = 25,
    SESSION_JOINED = 26,
    SESSION_LEFT = 27,
    PLAYER_TRIGGER = 28,
    CINEMATIC_STARTED = 29,
    CINEMATIC_TERMINATED = 30,
    ACTOR_PATH_STATE = 31,
    GHOST_LINK_STATE = 32,
    OBJECT_INTERACTED = 33,
    CINEMATIC_SKIP_REQUESTED = 34,
    FIRETEAM_STATE = 35,
    OBJECT_STATE = 36,
    DAMAGE_STATE = 37,
    SQUAD_PROVOKED = 38,
    DEVICE_STATE = 39,
}

---@class SunriseEvent
---@field kind SunriseEventKind
---@field sequence string
---@field source_generation string

---@alias SunriseEventHandler fun(context: any, state: SunriseState, event: SunriseEvent)

---@class SunrisePlayerTriggerEvent: SunriseEvent
---@field registry_key integer
---@field object_tag integer
---@field slot_type integer
---@field slot_index integer
---@field volume_registry_key integer
---@field volume_slot_type integer
---@field volume_slot_index integer
---@field resolved_object_id integer

---@class SunriseCinematicEvent: SunriseEvent
---@field registry_key integer
---@field object_tag integer
---@field slot_type integer
---@field slot_index integer
---@field runtime_object_id string
---@field event_value number

---@class SunriseEntitySlotsRequestedEvent: SunriseEvent
---@field requested_count integer

---@class SunriseDeviceStateEvent: SunriseEvent
---@field registry_key integer
---@field object_tag integer
---@field slot_type integer
---@field slot_index integer
---@field position number?
---@field power number?
---@field lock number?
---@field position_sequence integer?
---@field power_sequence integer?
---@field lock_sequence integer?
---@field first_report boolean
---@field reset boolean

---@class SunriseProgram
---@field on_start? fun(context: any, state: SunriseState)
---@field on_load? fun(context: any, state: SunriseState)
---@field on_event_sensor_sense_updated? SunriseEventHandler
---@field on_event_client_state_changed? SunriseEventHandler
---@field on_event_client_message_received? SunriseEventHandler
---@field on_event_auth_state_committed? SunriseEventHandler
---@field on_event_auth_state_transport_staged? SunriseEventHandler
---@field on_event_auth_state_canceled? SunriseEventHandler
---@field on_event_incident_received? SunriseEventHandler
---@field on_event_incident_queued? SunriseEventHandler
---@field on_event_incident_transport_staged? SunriseEventHandler
---@field on_event_incident_canceled? SunriseEventHandler
---@field on_event_incident_refused? SunriseEventHandler
---@field on_event_scriptable_override_committed? SunriseEventHandler
---@field on_event_scriptable_override_transport_staged? SunriseEventHandler
---@field on_event_scriptable_override_canceled? SunriseEventHandler
---@field on_event_operator_refused? SunriseEventHandler
---@field on_event_timer_elapsed? SunriseEventHandler
---@field on_event_effect_result? SunriseEventHandler
---@field on_event_phase_entered? SunriseEventHandler
---@field on_event_trigger_entered? SunriseEventHandler
---@field on_event_trigger_exited? SunriseEventHandler
---@field on_event_squad_state? SunriseEventHandler
---@field on_event_device_state? fun(ctx: any, state: SunriseState, event: SunriseDeviceStateEvent)
---@field on_event_entity_spawned? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_entity_died? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_scene_finished? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_objective_progress? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_entity_slots_requested? fun(context: any, state: SunriseState, )lua"
            R"lua(event: SunriseEntitySlotsRequestedEvent)
---@field on_event_session_joined? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_session_left? fun(context: any, state: SunriseState, event: SunriseEvent)
---@field on_event_player_trigger? fun(context: any, state: SunriseState, )lua"
            R"lua(event: SunrisePlayerTriggerEvent)
---@field on_event_cinematic_started? fun(context: any, state: SunriseState, )lua"
            R"lua(event: SunriseCinematicEvent)
---@field on_event_cinematic_terminated? fun(context: any, state: SunriseState, )lua"
            R"lua(event: SunriseCinematicEvent)
---@field on_event_actor_path_state? SunriseEventHandler
---@field on_event_ghost_link_state? SunriseEventHandler
---@field on_event_object_interacted? SunriseEventHandler
---@field on_event_fireteam_state? SunriseEventHandler
---@field on_event_object_state? SunriseEventHandler
---@field on_event_damage_state? SunriseEventHandler
---@field on_event_cinematic_skip_requested? fun(context: any, state: SunriseState, )lua"
            R"lua(event: SunriseCinematicEvent)

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
---@field set_object_active fun(SunriseSlot, SunriseObjectArguments?): SunriseRequestKey
---@field assign_combat_objective fun(self: SunriseSlot, args: {objective: SunriseSlot, )lua"
            R"lua(revision: integer, task_group: integer, reserved: boolean?, )lua"
            R"lua(refresh_player_awareness: boolean?}): SunriseRequestKey
---@field play_actor_path fun(self: SunriseSlot, args: {generation: integer, )lua"
            R"lua(revision: integer, path: SunriseSlot}): SunriseRequestKey
---@field deliver_squads fun(self: SunriseSlot, args: {generation: integer, )lua"
            R"lua(revision: integer, squads: SunriseSlot[]}): SunriseRequestKey
---@field deliver_squad fun(self: SunriseSlot, args: {generation: integer, revision: integer, )lua"
            R"lua(squad: SunriseSlot}): SunriseRequestKey
---@field play_actor_action fun(self: SunriseSlot, args: {generation: integer, )lua"
            R"lua(revision: integer, group: integer, action: integer}): SunriseRequestKey
---@field retire_actor fun(self: SunriseSlot, args: {generation: integer}): SunriseRequestKey
---@field set_darkness_zone fun(self: SunriseSlot, args: {enabled: boolean, )lua"
            R"lua(wipe_seconds: integer?}): SunriseRequestKey
---@field set_interactable_object fun(self: SunriseSlot, args: {generation: integer, )lua"
            R"lua(track_owner: boolean?, active: boolean?}): SunriseRequestKey
---@field set_music_section fun(self: SunriseSlot, args: {section: integer, )lua"
            R"lua(enabled: boolean?}): SunriseRequestKey
---@field watch_damage fun(self: SunriseSlot, args: {target: SunriseSlot, )lua"
            R"lua(revision: integer}): SunriseRequestKey
---@field set_object_filter fun(self: SunriseSlot, args: {players: boolean?, )lua"
            R"lua(target: SunriseSlot?, inside: SunriseSlot?, )lua"
            R"lua(inside_any: SunriseSlot[]?}): SunriseRequestKey
---@field set_mission_effect fun(self: SunriseSlot, args: {filter: SunriseSlot?, )lua"
            R"lua(enabled: boolean, revision: integer}): SunriseRequestKey
---@field set_ghost_link fun(self: SunriseSlot, args: {generation: integer, )lua"
            R"lua(enabled: boolean}): SunriseRequestKey
---@field bind_combatant_to_squad fun(self: SunriseSlot): SunriseRequestKey
---@field transition fun(SunriseSlot, SunriseDeviceTransitionArguments): SunriseRequestKey
---@field fire_trigger fun(self: SunriseSlot): SunriseRequestKey
---@field sequences fun(self: SunriseSlot): SunriseActorSequences
---@field play_sequence fun(self: SunriseSlot, args: {sequence: SunriseActorSequence}): )lua"
            R"lua(SunriseRequestKey
---@field set_cinematic_active fun(SunriseSlot, SunriseCinematicArguments?): SunriseRequestKey
---@field reset_objectives fun(self: SunriseSlot): SunriseRequestKey
---@field advance_task fun(self: SunriseSlot): SunriseRequestKey
---@field play_dialogue_cue fun(SunriseSlot, SunriseDialogueCueArguments): SunriseRequestKey
---@field set_directive fun(SunriseSlot, SunriseDirectiveArguments): SunriseRequestKey
---@field clear_directives fun(self: SunriseSlot): SunriseRequestKey
---@field set_engagement_state fun(SunriseSlot, SunriseEngagementArguments): SunriseRequestKey
---@field set_public_event_state fun(SunriseSlot, SunrisePublicEventArguments): SunriseRequestKey

---@class SunriseRequestKey

---@class SunriseCinematicArguments
---@field active? boolean

---@class SunriseObjectArguments
---@field active? boolean


---@class SunriseDialogueCueArguments
---@field cue integer

---@class SunriseDeviceTransitionArguments
---@field transition any Generated device transition value.

---@class SunriseDirectiveArguments
---@field directive table Generated mission directive declaration.
---@field state? integer Defaults to 0, the native enter state.
---@field audience? SunriseSlot Authored type-70 engagement sensor for the mission banner.
---@field navpoint? SunriseSlot Authored type-47 navigation marker.

---@class SunriseEngagementArguments
---@field flags? integer Five native flag bits; the shipped constructor default is 1.
---@field revision? integer Signed Sense-list revision; the shipped constructor default is 1.

---@class SunrisePublicEventArguments
---@field state? integer Shown unchanged by the HUD directive that names this sensor. Defaults to 0.
---@field player? string Decimal player key of the watched player. Defaults to context.player_key.
---@field area SunriseSlot The object whose zone list bounds the event area.
---@field leave_seconds number Seconds outside that area before the client reports Sense 1.

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
---@field Task table<string, string>
---@field TriggerVolume table<string, SunriseTriggerVolume>

---@class SunriseActivity
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

sdk.CombatantLanePrimarySchema = {
    REF_BYTE = 0x80807F7F,
    U32 = 0x80807F7E,
    REAL32 = 0x80807F7D,
    REF_BYTE_BOOL = 0x80807F7A,
    EMPTY = 0x80807F79,
    U6 = 0x80807F80,
    U32_BOOL = 0x80807F78,
    U32_REAL32 = 0x80807F81,
    ALTERNATE_REF_BYTE = 0x80807F77,
    TRIPLE_U32_REF_MODES = 0x80807F76,
}

sdk.CombatantLaneSecondarySchema = {
    EMPTY_A = 0x80807F8B,
    EMPTY_B = 0x80807F8A,
    QUANTIZED_11 = 0x80807F89,
}

-- Authored behavior-condition classes embedded in package behavior graphs.
sdk.AuthoredBehaviorFilterClass = {
    ACTIVITY_FLAG = 0x80804D83,
    ACTOR = 0x80804D82,
    AIRBORNE = 0x80804D81,
    CHANNEL = 0x80804D7D,
    HAS_PARENT = 0x80804D80,
    DAMAGE_OWNER = 0x80804D7C,
    DEAD = 0x80804D7B,
    DISTANCE = 0x80804D7A,
    FACTION = 0x80804D78,
    FACTION_RELATIONSHIP = 0x80804D76,
    HOP_ON = 0x80804D75,
    LINE_OF_SIGHT = 0x80804D74,
    OBJECT_LABEL = 0x80804D73,
    OBJECT_TYPE = 0x80804D72,
    PLAYER = 0x80804D71,
    RESOURCE = 0x80804D5F,
    SAME_OBJECT = 0x80802F95,
    SCOREBOARD_STAT = 0x80804D5E,
    TEAM_SIDE = 0x80804D5D,
    COMPOSITE = 0x80809310,
    EXTERNAL = 0x8080930F,
}

-- Compact code-34 Auth predicate schemas. These are a separate runtime registry.
sdk.ObjectFilterPredicateSchema = {
    MODE_U32_A = 0x80809571,
    MODE_U32_B = 0x80809572,
    MODE_ONLY_A = 0x80809573,
    UNREGISTERED_MODE_SLOT_REF = 0x80809574,
    MODE_U32_C = 0x80809575,
    MODE_FLAG_SLOT_REF = 0x80809576,
    MODE_SLOT_REF_A = 0x80809577,
    MODE_SLOT_REF_B = 0x80809578,
    MODE_SLOT_REF_C = 0x80809579,
    MODE_I32 = 0x8080957A,
    MODE_SLOT_REF_D = 0x8080957B,
    MODE_U32_D = 0x8080957C,
    MODE_ONLY_B = 0x8080957D,
}

---@class SunriseObjectFilterModeOnly
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
        return true;
    } catch (...) {
        output.manifestJson.clear();
        output.activitySdkModule.clear();
        output.behaviorModule.clear();
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
