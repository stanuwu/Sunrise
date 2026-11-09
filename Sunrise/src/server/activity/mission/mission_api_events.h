#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "../../../state/activity/mission/definition.h"
#include "../../../state/activity_sdk/format.h"
#include "mission_api_types.h"
#include "mission_api_values.h"

// Event views and the program table. Each event kind serves the field groups listed for it, in
// EventKind order; the callback name is the program key the VM captures for that kind.

namespace sunrise::server::activity::mission::api {

/** Index of each field group; the VM keeps one binding table per group in this order. */
enum class FieldGroupId : std::uint8_t {
    common,
    missionSequence,
    stateRevision,
    slotIdentity,
    firstObject,
    clientInput,
    peerHeard,
    senseDecode,
    clientState,
    incidentBody,
    clientMessage,
    scriptableRevision,
    incidentRevision,
    effect,
    region,
    playerTrigger,
    cinematic,
    trigger,
    squadAlive,
    squadState,
    entitySpawned,
    sceneFinished,
    sceneOutput,
    objectiveProgress,
    phase,
    timer,
    entitySlots,
    sessionPeer,
    joinedRevision,
    actorPath,
    device,
    damage,
    object,
    ghostLink,
    squadProvoked,
    fireteam,
    senseGeneration,
    squadRemoval,
    triggerLevel,
    count,
};

// --- Field shapes ------------------------------------------------------------------------------

// Event-only shapes; the shared ones live with the values.
inline constexpr ValueSpec kEventKind[]{
    {.kind = ValueKind::integer, .ref = "EventKind", .note = "the EventKind number"}};
inline constexpr ValueSpec kOptionalEventSlot[]{
    {.kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .note = "nil when the slot is not in this activity"}};
inline constexpr ValueSpec kFirstObjectInteger[]{
    {.kind = ValueKind::integer, .optional = true, .note = "nil when the update named no object"}};
inline constexpr ValueSpec kFirstObjectSlot[]{
    {.kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .note = "nil when the update named no object or the slot is not in this activity"}};
inline constexpr ValueSpec kOptionalMessage[]{{.kind = ValueKind::handle,
                                               .ref = "activity_message",
                                               .optional = true,
                                               .note = "the catalog message row; a live SDK view"}};
inline constexpr ValueSpec kMessageStatus[]{
    {.kind = ValueKind::string,
     .note = "unclassified, decoded, decoded_partial, prefix_only, opaque, outer_decoded, "
             "prepared, prepare_refused, malformed or quarantined"}};
inline constexpr ValueSpec kEffectName[]{
    {.kind = ValueKind::string,
     .optional = true,
     .note = "the request's effect name; nil for a request without one"}};
inline constexpr ValueSpec kEffectOutcome[]{
    {.kind = ValueKind::string,
     .optional = true,
     .note = "transport_staged, refused, expired or canceled"}};
inline constexpr ValueSpec kOutcomeCode[]{
    {.kind = ValueKind::integer, .bound = between(0, 3), .note = "the outcome in the same order"}};
inline constexpr ValueSpec kEnteredFlag[]{
    {.kind = ValueKind::boolean,
     .optional = true,
     .note = "true only on the host's arrival answer; nil otherwise"}};
inline constexpr ValueSpec kTaskGroupArguments[]{
    {.name = "group", .kind = ValueKind::missionRow, .ref = "mission.TaskGroup"}};
inline constexpr ValueSpec kTaskCostReturns[]{
    {.name = "cost", .kind = ValueKind::number, .optional = true},
    {.name = "known", .kind = ValueKind::boolean},
};
inline constexpr ValueSpec kTaskGroupQueryArguments[]{
    {.name = "objective",
     .kind = ValueKind::handle,
     .ref = "slot",
     .slot = type_gate(state::activity_sdk::format::kObjectiveSlotType)},
};
inline constexpr ValueSpec kTaskGroupReturns[]{
    {.name = "group",
     .kind = ValueKind::missionRow,
     .ref = "mission.TaskGroup",
     .optional = true,
     .note = "compares equal to a mission.TaskGroup entry"},
    {.name = "assigned", .kind = ValueKind::boolean},
};
inline constexpr ValueSpec kAppliedRequestArguments[]{
    {.name = "channel", .kind = ValueKind::enumValue, .ref = "device_channels"}};
inline constexpr ValueSpec kTaskCosts[]{
    {.kind = ValueKind::table,
     .element = kNumber,
     .note = "reported cost per task group, keyed from 1; a group without one is absent"}};
inline constexpr ValueSpec kContinuity[]{
    {.kind = ValueKind::string,
     .note = "baseline, consecutive, duplicate, conflict, gap, reset, missing_counter, wrap or "
             "invalid"}};
inline constexpr ValueSpec kOptionalBoolean[]{{.kind = ValueKind::boolean, .optional = true}};
inline constexpr std::string_view kUnavailableLevel = "nil when available is false";

// --- Field groups ------------------------------------------------------------------------------
// Members of each group, then the group itself.
inline constexpr MemberSpec kCommonFields[]{
    field("kind", kEventKind),
    field("sequence", kDecimal, "event order number"),
    field("source_generation", kDecimal, "client generation that produced it"),
    field("attempt_generation", kDecimal, "attempt it belongs to"),
};
inline constexpr MemberSpec kMissionSequenceFields[]{
    field("mission_sequence", kDecimal, "the input order")};
inline constexpr MemberSpec kStateRevisionFields[]{field("state_revision", kDecimal)};
inline constexpr MemberSpec kSlotIdentityFields[]{
    field("registry_key", kInteger),
    field("object_tag", kInteger),
    field("slot_index", kInteger),
    field("slot_type", kInteger),
    field("slot", kOptionalEventSlot),
};
inline constexpr MemberSpec kFirstObjectFields[]{
    field("registry_key", kFirstObjectInteger),
    field("object_tag", kFirstObjectInteger),
    field("slot_index", kFirstObjectInteger),
    field("slot_type", kFirstObjectInteger),
    field("slot", kFirstObjectSlot),
};
inline constexpr MemberSpec kClientInputFields[]{
    field("client_message_sequence", kDecimal),
    field("payload_bytes", kInteger),
};
inline constexpr MemberSpec kPeerHeardFields[]{field("peer_heard_mask", kInteger)};
inline constexpr MemberSpec kSenseDecodeFields[]{
    field("objects_decoded", kInteger),
    field("groups_decoded", kInteger),
};
inline constexpr MemberSpec kClientStateFields[]{
    field("activity_state_revision", kDecimal),
    field("membership_revision", kInteger),
    field("region_index", kOptionalInteger, "present when the report names a region leg"),
    field("current_region_index", kOptionalInteger, "present when it names the current region"),
    field("held_region_index", kOptionalInteger, "present when the host knows the held region"),
    field("region_slice_set_hash", kOptionalInteger, "present with region_index"),
    field("spawn_state", kOptionalInteger, "present when the report carries a spawn byte"),
    field("teleport_state", kOptionalInteger, "present when it carries a teleport byte"),
    field("teleport_slice_set_index", kOptionalInteger, "present with teleport_state"),
    field("teleport_slice_set_hash", kOptionalInteger, "present with teleport_state"),
    field("entered", kEnteredFlag),
};
inline constexpr MemberSpec kIncidentBodyFields[]{
    field("incident_target", kInteger),
    field("incident_extra_targets", kInteger),
    field("incident_selector_bytes", kInteger),
    field("incident_payload_bytes", kInteger),
};
inline constexpr MemberSpec kClientMessageFields[]{
    field("message_type", kInteger, "the message id"),
    field("message_status", kMessageStatus),
    field("message_name", kOptionalString, "the catalog name"),
    field("message", kOptionalMessage),
};
inline constexpr MemberSpec kScriptableRevisionFields[]{field("scriptable_revision", kDecimal)};
inline constexpr MemberSpec kIncidentRevisionFields[]{field("incident_revision", kDecimal)};
inline constexpr MemberSpec kEffectFields[]{
    field("request_key", kRequestKey),
    field("effect", kEffectName),
    field("outcome", kEffectOutcome),
    field("outcome_code", kOutcomeCode),
};
inline constexpr MemberSpec kRegionFields[]{
    field("region_index", kInteger),
    field("previous_region_index", kOptionalInteger, "nil the first time"),
};
inline constexpr MemberSpec kPlayerTriggerFields[]{
    field("volume_registry_key", kInteger),
    field("volume_slot_type", kInteger),
    field("volume_slot_index", kInteger),
    field("resolved_object_id", kInteger),
};
inline constexpr MemberSpec kCinematicFields[]{
    field("runtime_object_id", kDecimal),
    field("event_value", kNumber),
};
inline constexpr MemberSpec kTriggerFields[]{
    field("member_count", kInteger),
    field("value", kInteger),
    field("all_inside", kBoolean),
};
inline constexpr MemberSpec kSquadAliveFields[]{
    field("alive_count", kInteger),
    field("previous_alive_count", kInteger),
};
inline constexpr MemberSpec kSquadStateFields[]{
    {.name = "task_cost",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .arguments = kTaskGroupArguments,
     .returns = kTaskCostReturns,
     .note = "the reachable cost this report gives the group"},
    {.name = "task_group",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .arguments = kTaskGroupQueryArguments,
     .returns = kTaskGroupReturns,
     .note = "the group the squad holds for this objective"},
    field("slot_counts", kIntegerList),
    field("initial_observation", kBoolean, "the first report of this squad; never a death"),
    field("population_available",
          kBoolean,
          "false until an alive count is reported; until then the counts are zero placeholders"),
    field("objective_revision",
          kInteger,
          "objective revision the squad echoed with its costs; 0 before any"),
    field("task_costs", kTaskCosts),
};
inline constexpr MemberSpec kEntitySpawnedFields[]{
    field("member_slot", kInteger),
    field("count", kInteger),
    field("previous_count", kInteger),
};
inline constexpr MemberSpec kSceneFinishedFields[]{field("activation_token", kInteger)};
inline constexpr MemberSpec kSceneOutputFields[]{
    field("key", kInteger, "the output key"),
    field("activation_token", kInteger, "the run that reported it"),
};
inline constexpr MemberSpec kObjectiveProgressFields[]{
    field("objective", kInteger),
    field("task", kInteger),
    field("task_count", kInteger),
    field("previous_task_count", kInteger),
};
inline constexpr MemberSpec kPhaseFields[]{
    field("phase", kInteger),
    field("previous_phase", kInteger),
};
inline constexpr MemberSpec kTimerFields[]{
    field("timer_name", kString),
    field("timer_deadline_tick", kDecimal),
    field("timer_sequence", kDecimal),
};
inline constexpr MemberSpec kEntitySlotsFields[]{field("requested_count", kInteger)};
inline constexpr MemberSpec kSessionPeerFields[]{
    field("session_id", kDecimal),
    field("session_generation", kDecimal),
    field("member_key", kDecimal),
};
inline constexpr MemberSpec kJoinedRevisionFields[]{
    field("joined_revision", kDecimal, "mission state revision at the join")};
inline constexpr MemberSpec kActorPathFields[]{
    field("generation", kInteger),
    field("revision", kInteger),
    field("path_state", kInteger),
    field("delivery_revision", kOptionalInteger, "nil until the delivery is known"),
    field("delivery_state", kOptionalInteger, "nil until the delivery is known"),
    field("suppressed", kBoolean),
};
inline constexpr std::string_view kNotCarried = "nil when this report did not carry it";
inline constexpr MemberSpec kDeviceFields[]{
    field("position", kOptionalNumber, kNotCarried),
    field("power", kOptionalNumber, kNotCarried),
    field("lock", kOptionalNumber, kNotCarried),
    field("position_sequence", kOptionalInteger, kNotCarried),
    field("power_sequence", kOptionalInteger, kNotCarried),
    field("lock_sequence", kOptionalInteger, kNotCarried),
    field("first_report", kBoolean),
    field("reset", kBoolean),
    {.name = "applied_request",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .arguments = kAppliedRequestArguments,
     .returns = kOptionalRequestKey,
     .note = "the request this report satisfied on the channel, or nil"},
};
inline constexpr MemberSpec kDamageFields[]{
    field("health", kNumber),
    field("shield", kNumber),
    field("revision", kInteger),
    field("from_entity", kBoolean, "true when a replicated entity dealt the damage"),
    field("health_level", kOptionalInteger, "the entity's health pool level, when known"),
    field("shield_level", kOptionalInteger, "the entity's shield pool level, when known"),
    field("entity_slot", kOptionalInteger, "nil unless from_entity"),
    field("entity_incarnation", kOptionalInteger, "nil unless from_entity"),
    field("allocation_sequence", kOptionalInteger, "nil unless from_entity"),
    field("allocation_domain", kOptionalDecimal, "nil unless from_entity"),
};
inline constexpr MemberSpec kObjectFields[]{
    field("generation", kInteger),
    field("present", kBoolean),
    field("alive", kBoolean, "same as present"),
    field("interaction_open", kBoolean, "Sense ordinal 1; meaning assumed"),
    field("owner_known", kBoolean),
    field("has_owner", kBoolean),
    field("owner_key", kOptionalDecimal, "nil without an owner"),
    field("initial_observation", kBoolean, "first report of this generation"),
    field("entry_index", kInteger, "authored entry the object instantiated"),
    field("spawn_mask_low", kInteger),
    field("spawn_mask_high", kInteger),
};
inline constexpr MemberSpec kGhostLinkFields[]{
    field("generation", kInteger),
    field("progress", kNumber),
    field("active", kBoolean),
};
inline constexpr MemberSpec kSquadProvokedFields[]{
    field("registry_key", kInteger),
    field("slot_type", kInteger),
    field("slot_index", kInteger),
};
inline constexpr MemberSpec kFireteamFields[]{
    field("alive_count", kInteger),
    field("dead_count", kInteger),
    field("unknown_count", kInteger),
};
inline constexpr MemberSpec kSenseGenerationFields[]{
    field("spawn_generation",
          kOptionalInteger,
          "spawn generation the squad Sense echoes from its placement; nil when unreported"),
    field("sense_generation", kOptionalInteger, "msg-6 record counter plus one; nil when absent"),
};
inline constexpr MemberSpec kSquadRemovalFields[]{
    field("removal_flag", kBoolean, "false when the report did not carry it")};
inline constexpr MemberSpec kTriggerLevelFields[]{
    field("available", kBoolean, "the report carried a complete occupancy level"),
    field("continuity", kContinuity),
    field("occupied", kOptionalBoolean, kUnavailableLevel),
    field("member_count", kOptionalInteger, kUnavailableLevel),
    field("value", kOptionalInteger, kUnavailableLevel),
    field("all_inside", kOptionalBoolean, kUnavailableLevel),
};

/** Every field group, indexed by FieldGroupId. */
inline constexpr std::array<FieldGroup, static_cast<std::size_t>(FieldGroupId::count)> kFieldGroups{
    {
        {"common", kCommonFields},
        {"mission_sequence", kMissionSequenceFields},
        {"state_revision", kStateRevisionFields},
        {"slot_identity", kSlotIdentityFields},
        {"first_object", kFirstObjectFields},
        {"client_input", kClientInputFields},
        {"peer_heard", kPeerHeardFields},
        {"sense_decode", kSenseDecodeFields},
        {"client_state", kClientStateFields},
        {"incident_body", kIncidentBodyFields},
        {"client_message", kClientMessageFields},
        {"scriptable_revision", kScriptableRevisionFields},
        {"incident_revision", kIncidentRevisionFields},
        {"effect", kEffectFields},
        {"region", kRegionFields},
        {"player_trigger", kPlayerTriggerFields},
        {"cinematic", kCinematicFields},
        {"trigger", kTriggerFields},
        {"squad_alive", kSquadAliveFields},
        {"squad_state", kSquadStateFields},
        {"entity_spawned", kEntitySpawnedFields},
        {"scene_finished", kSceneFinishedFields},
        {"scene_output", kSceneOutputFields},
        {"objective_progress", kObjectiveProgressFields},
        {"phase", kPhaseFields},
        {"timer", kTimerFields},
        {"entity_slots", kEntitySlotsFields},
        {"session_peer", kSessionPeerFields},
        {"joined_revision", kJoinedRevisionFields},
        {"actor_path", kActorPathFields},
        {"device", kDeviceFields},
        {"damage", kDamageFields},
        {"object", kObjectFields},
        {"ghost_link", kGhostLinkFields},
        {"squad_provoked", kSquadProvokedFields},
        {"fireteam", kFireteamFields},
        {"sense_generation", kSenseGenerationFields},
        {"squad_removal", kSquadRemovalFields},
        {"trigger_level", kTriggerLevelFields},
    }};

/** @return The address of one field group, for an event's group list. */
[[nodiscard]] consteval const FieldGroup* group(FieldGroupId id) {
    return &kFieldGroups[static_cast<std::size_t>(id)];
}

// --- Event group lists -------------------------------------------------------------------------
using G = FieldGroupId;
// One list per event shape; kinds that share a shape share a list.
inline constexpr const FieldGroup* kSenseUpdatedGroups[]{group(G::common),
                                                         group(G::missionSequence),
                                                         group(G::clientInput),
                                                         group(G::stateRevision),
                                                         group(G::peerHeard),
                                                         group(G::senseDecode),
                                                         group(G::firstObject)};
inline constexpr const FieldGroup* kClientStateGroups[]{
    group(G::common), group(G::missionSequence), group(G::clientInput), group(G::clientState)};
inline constexpr const FieldGroup* kClientMessageGroups[]{group(G::common),
                                                          group(G::missionSequence),
                                                          group(G::clientInput),
                                                          group(G::peerHeard),
                                                          group(G::stateRevision),
                                                          group(G::clientMessage)};
inline constexpr const FieldGroup* kStateRevisionGroups[]{group(G::common),
                                                          group(G::stateRevision)};
inline constexpr const FieldGroup* kIncidentReceivedGroups[]{
    group(G::common), group(G::missionSequence), group(G::clientInput), group(G::incidentBody)};
inline constexpr const FieldGroup* kIncidentDeliveryGroups[]{
    group(G::common), group(G::incidentRevision), group(G::incidentBody)};
inline constexpr const FieldGroup* kScriptableGroups[]{group(G::common),
                                                       group(G::scriptableRevision)};
inline constexpr const FieldGroup* kTimerGroups[]{
    group(G::common), group(G::missionSequence), group(G::timer)};
inline constexpr const FieldGroup* kEffectGroups[]{
    group(G::common), group(G::missionSequence), group(G::effect)};
inline constexpr const FieldGroup* kPhaseGroups[]{
    group(G::common), group(G::missionSequence), group(G::phase), group(G::stateRevision)};
inline constexpr const FieldGroup* kTriggerGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::trigger)};
inline constexpr const FieldGroup* kTriggerStateGroups[]{group(G::common),
                                                         group(G::missionSequence),
                                                         group(G::slotIdentity),
                                                         group(G::senseGeneration),
                                                         group(G::triggerLevel)};
inline constexpr const FieldGroup* kSquadStateGroups[]{group(G::common),
                                                       group(G::missionSequence),
                                                       group(G::slotIdentity),
                                                       group(G::senseGeneration),
                                                       group(G::squadAlive),
                                                       group(G::squadRemoval),
                                                       group(G::squadState)};
inline constexpr const FieldGroup* kEntitySpawnedGroups[]{group(G::common),
                                                          group(G::missionSequence),
                                                          group(G::slotIdentity),
                                                          group(G::senseGeneration),
                                                          group(G::entitySpawned)};
inline constexpr const FieldGroup* kEntityDiedGroups[]{group(G::common),
                                                       group(G::missionSequence),
                                                       group(G::slotIdentity),
                                                       group(G::senseGeneration),
                                                       group(G::squadAlive),
                                                       group(G::squadRemoval)};
inline constexpr const FieldGroup* kSceneFinishedGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::sceneFinished)};
inline constexpr const FieldGroup* kObjectiveProgressGroups[]{group(G::common),
                                                              group(G::missionSequence),
                                                              group(G::slotIdentity),
                                                              group(G::objectiveProgress)};
inline constexpr const FieldGroup* kEntitySlotsGroups[]{
    group(G::common), group(G::missionSequence), group(G::entitySlots)};
inline constexpr const FieldGroup* kSessionJoinedGroups[]{
    group(G::common), group(G::missionSequence), group(G::sessionPeer), group(G::joinedRevision)};
inline constexpr const FieldGroup* kSessionLeftGroups[]{
    group(G::common), group(G::missionSequence), group(G::sessionPeer)};
inline constexpr const FieldGroup* kPlayerTriggerGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::playerTrigger)};
inline constexpr const FieldGroup* kCinematicGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::cinematic)};
inline constexpr const FieldGroup* kGhostLinkGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::ghostLink)};
inline constexpr const FieldGroup* kActorPathGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::actorPath)};
inline constexpr const FieldGroup* kObjectGroups[]{group(G::common),
                                                   group(G::missionSequence),
                                                   group(G::slotIdentity),
                                                   group(G::senseGeneration),
                                                   group(G::object)};
inline constexpr const FieldGroup* kFireteamGroups[]{
    group(G::common), group(G::missionSequence), group(G::fireteam)};
inline constexpr const FieldGroup* kDamageGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::damage)};
inline constexpr const FieldGroup* kSquadProvokedGroups[]{
    group(G::common), group(G::missionSequence), group(G::squadProvoked)};
inline constexpr const FieldGroup* kDeviceGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::device)};
inline constexpr const FieldGroup* kRegionGroups[]{
    group(G::common), group(G::missionSequence), group(G::region)};
inline constexpr const FieldGroup* kSceneOutputGroups[]{
    group(G::common), group(G::missionSequence), group(G::slotIdentity), group(G::sceneOutput)};

/** Event kinds in EventKind order; the index is the kind. */
inline constexpr EventSpec kEvents[]{
    {0,
     "SENSOR_SENSE_UPDATED",
     "on_event_sensor_sense_updated",
     "SunriseSensorSenseUpdatedEvent",
     kSenseUpdatedGroups,
     "slot identity is that of the first decoded object"},
    {1,
     "CLIENT_STATE_CHANGED",
     "on_event_client_state_changed",
     "SunriseClientStateChangedEvent",
     kClientStateGroups,
     "most reports carry no change; react to entered for the arrival"},
    {2,
     "CLIENT_MESSAGE_RECEIVED",
     "on_event_client_message_received",
     "SunriseClientMessageReceivedEvent",
     kClientMessageGroups},
    {3,
     "AUTH_STATE_COMMITTED",
     "on_event_auth_state_committed",
     "SunriseAuthStateEvent",
     kStateRevisionGroups},
    {4,
     "AUTH_STATE_TRANSPORT_STAGED",
     "on_event_auth_state_transport_staged",
     "SunriseAuthStateEvent",
     kStateRevisionGroups},
    {5,
     "AUTH_STATE_CANCELED",
     "on_event_auth_state_canceled",
     "SunriseAuthStateEvent",
     kStateRevisionGroups},
    {6,
     "INCIDENT_RECEIVED",
     "on_event_incident_received",
     "SunriseIncidentReceivedEvent",
     kIncidentReceivedGroups},
    {7,
     "INCIDENT_QUEUED",
     "on_event_incident_queued",
     "SunriseIncidentDeliveryEvent",
     kIncidentDeliveryGroups},
    {8,
     "INCIDENT_TRANSPORT_STAGED",
     "on_event_incident_transport_staged",
     "SunriseIncidentDeliveryEvent",
     kIncidentDeliveryGroups},
    {9,
     "INCIDENT_CANCELED",
     "on_event_incident_canceled",
     "SunriseIncidentDeliveryEvent",
     kIncidentDeliveryGroups,
     "body members are 0 without a record"},
    {10,
     "INCIDENT_REFUSED",
     "on_event_incident_refused",
     "SunriseIncidentDeliveryEvent",
     kIncidentDeliveryGroups,
     "incident_revision is always 0"},
    {11,
     "SCRIPTABLE_OVERRIDE_COMMITTED",
     "on_event_scriptable_override_committed",
     "SunriseScriptableOverrideEvent",
     kScriptableGroups},
    {12,
     "SCRIPTABLE_OVERRIDE_TRANSPORT_STAGED",
     "on_event_scriptable_override_transport_staged",
     "SunriseScriptableOverrideEvent",
     kScriptableGroups},
    {13,
     "SCRIPTABLE_OVERRIDE_CANCELED",
     "on_event_scriptable_override_canceled",
     "SunriseScriptableOverrideEvent",
     kScriptableGroups},
    {14,
     "OPERATOR_REFUSED",
     "on_event_operator_refused",
     "SunriseAuthStateEvent",
     kStateRevisionGroups,
     "state_revision is 0 on most refusals"},
    {15, "TIMER_ELAPSED", "on_event_timer_elapsed", "SunriseTimerElapsedEvent", kTimerGroups},
    {16, "EFFECT_RESULT", "on_event_effect_result", "SunriseEffectResultEvent", kEffectGroups},
    {17, "PHASE_ENTERED", "on_event_phase_entered", "SunrisePhaseEnteredEvent", kPhaseGroups},
    {18, "TRIGGER_ENTERED", "on_event_trigger_entered", "SunriseTriggerEvent", kTriggerGroups},
    {19, "TRIGGER_EXITED", "on_event_trigger_exited", "SunriseTriggerEvent", kTriggerGroups},
    {20, "SQUAD_STATE", "on_event_squad_state", "SunriseSquadStateEvent", kSquadStateGroups},
    {21,
     "ENTITY_SPAWNED",
     "on_event_entity_spawned",
     "SunriseEntitySpawnedEvent",
     kEntitySpawnedGroups},
    {22, "ENTITY_DIED", "on_event_entity_died", "SunriseEntityDiedEvent", kEntityDiedGroups},
    {23,
     "SCENE_FINISHED",
     "on_event_scene_finished",
     "SunriseSceneFinishedEvent",
     kSceneFinishedGroups},
    {24,
     "OBJECTIVE_PROGRESS",
     "on_event_objective_progress",
     "SunriseObjectiveProgressEvent",
     kObjectiveProgressGroups},
    {25,
     "ENTITY_SLOTS_REQUESTED",
     "on_event_entity_slots_requested",
     "SunriseEntitySlotsRequestedEvent",
     kEntitySlotsGroups},
    {26,
     "SESSION_JOINED",
     "on_event_session_joined",
     "SunriseSessionJoinedEvent",
     kSessionJoinedGroups},
    {27, "SESSION_LEFT", "on_event_session_left", "SunriseSessionLeftEvent", kSessionLeftGroups},
    {28,
     "PLAYER_TRIGGER",
     "on_event_player_trigger",
     "SunrisePlayerTriggerEvent",
     kPlayerTriggerGroups},
    {29,
     "CINEMATIC_STARTED",
     "on_event_cinematic_started",
     "SunriseCinematicEvent",
     kCinematicGroups},
    {30,
     "CINEMATIC_TERMINATED",
     "on_event_cinematic_terminated",
     "SunriseCinematicEvent",
     kCinematicGroups},
    {31,
     "GHOST_LINK_STATE",
     "on_event_ghost_link_state",
     "SunriseGhostLinkStateEvent",
     kGhostLinkGroups},
    {32,
     "ACTOR_PATH_STATE",
     "on_event_actor_path_state",
     "SunriseActorPathStateEvent",
     kActorPathGroups},
    {33,
     "OBJECT_INTERACTED",
     "on_event_object_interacted",
     "SunriseObjectStateEvent",
     kObjectGroups},
    {34,
     "CINEMATIC_SKIP_REQUESTED",
     "on_event_cinematic_skip_requested",
     "SunriseCinematicEvent",
     kCinematicGroups},
    {35, "FIRETEAM_STATE", "on_event_fireteam_state", "SunriseFireteamStateEvent", kFireteamGroups},
    {36, "OBJECT_STATE", "on_event_object_state", "SunriseObjectStateEvent", kObjectGroups},
    {37, "DAMAGE_STATE", "on_event_damage_state", "SunriseDamageStateEvent", kDamageGroups},
    {38,
     "SQUAD_PROVOKED",
     "on_event_squad_provoked",
     "SunriseSquadProvokedEvent",
     kSquadProvokedGroups,
     "carries the squad reference but no slot handle"},
    {39, "DEVICE_STATE", "on_event_device_state", "SunriseDeviceStateEvent", kDeviceGroups},
    {40, "REGION_CHANGED", "on_event_region_changed", "SunriseRegionChangedEvent", kRegionGroups},
    {41, "SCENE_OUTPUT", "on_event_scene_output", "SunriseSceneOutputEvent", kSceneOutputGroups},
    {42,
     "TRIGGER_STATE",
     "on_event_trigger_state",
     "SunriseTriggerStateEvent",
     kTriggerStateGroups,
     "every accepted occupancy report and its counter continuity; it never changes when "
     "entered and exited fire"},
};

// --- Program table -----------------------------------------------------------------------------

// The fields of program.initial_state.
inline constexpr ValueSpec kInitialStateFields[]{
    {.name = "region_index", .kind = ValueKind::integer, .bound = between(0, kInt32Maximum)},
    {.name = "spawn_set_hash",
     .kind = ValueKind::integer,
     .optional = true,
     .bound = between(1, kUint32Maximum),
     .note = "the spawn-point filter of the arrival and every host move"},
    {.name = "omit",
     .kind = ValueKind::list,
     .optional = true,
     .bound = at_most(
         static_cast<std::int64_t>(::sunrise::state::activity::mission::kMissionSeedOmitCapacity)),
     .element = kSlotSelector,
     .note = "objects kept out of every seed"},
};

/** Keys of the table a mission chunk returns, beside the event callbacks. */
inline constexpr ValueSpec kProgramEntries[]{
    {.name = "initial_state",
     .kind = ValueKind::table,
     .ref = "mission.states",
     .optional = true,
     .fields = kInitialStateFields,
     .note = "a generated mission.states row; the arrival loads it"},
    {.name = "on_start",
     .kind = ValueKind::function,
     .optional = true,
     .note = "on_start(context, state) runs once when the program starts"},
    {.name = "on_load",
     .kind = ValueKind::function,
     .optional = true,
     .note = "on_load(context, state) runs instead of on_start when an attach restores it"},
};

/** Program keys the VM refuses; one on_event_<name> callback per kind replaces on_event. */
inline constexpr std::array<std::string_view, 1> kRefusedProgramKeys{"on_event"};

} // namespace sunrise::server::activity::mission::api
