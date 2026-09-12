#pragma once

#include <string_view>

namespace sunrise::server::activity::mission::lua_vm::detail {

// Every metatable name the mission API registers, and the mission ABI ids two intents are named by.
inline constexpr char kActivityMetatable[] = "sunrise.sdk.activity";
inline constexpr char kActivityBindingTagCollectionMetatable[] =
    "sunrise.sdk.activity_binding_tags";
// Every binding locator of the bound activity.
inline constexpr char kActivityBindingLocatorCollectionMetatable[] =
    "sunrise.sdk.activity_binding_locators";
// One binding locator row, then the catalog collections a program indexes.
inline constexpr char kActivityBindingLocatorMetatable[] = "sunrise.sdk.activity_binding_locator";
inline constexpr char kSquadCollectionMetatable[] = "sunrise.sdk.squads";
inline constexpr char kSceneCollectionMetatable[] = "sunrise.sdk.authored_scenes";
inline constexpr char kSlotCollectionMetatable[] = "sunrise.sdk.slots";
inline constexpr char kMessageCollectionMetatable[] = "sunrise.sdk.activity_messages";
inline constexpr char kMessageFieldCollectionMetatable[] = "sunrise.sdk.activity_message_fields";
inline constexpr char kContextMetatable[] = "sunrise.mission.context";
inline constexpr char kStateMetatable[] = "sunrise.mission.state";
inline constexpr char kLifetimeMetatable[] = "sunrise.mission.lifetime";

// One event view per EventKind. Each name carries the kEventMetatablePrefix that check_event
// tests for, so any view is accepted where an event argument is asked for.
inline constexpr char kSenseUpdateEventMetatable[] = "sunrise.mission.event.sense_update";
inline constexpr char kClientStateChangedEventMetatable[] =
    "sunrise.mission.event.client_state_changed";
// Client-sent traffic: one arrived incident, then one routed message.
inline constexpr char kIncidentReceivedEventMetatable[] = "sunrise.mission.event.incident_received";
inline constexpr char kClientMessageEventMetatable[] =
    "sunrise.mission.event.client_message_received";
// Auth state, one view per stage of its commit.
inline constexpr char kAuthStateCommittedEventMetatable[] =
    "sunrise.mission.event.auth_state_committed";
// Auth state has reached transport.
inline constexpr char kAuthStateTransportStagedEventMetatable[] =
    "sunrise.mission.event.auth_state_transport_staged";
// Auth state was dropped before transport.
inline constexpr char kAuthStateCanceledEventMetatable[] =
    "sunrise.mission.event.auth_state_canceled";
// Incident, one view per stage of its commit.
inline constexpr char kIncidentQueuedEventMetatable[] = "sunrise.mission.event.incident_queued";
inline constexpr char kIncidentTransportStagedEventMetatable[] =
    "sunrise.mission.event.incident_transport_staged";
// Incident was dropped, and the scriptable override that follows it.
inline constexpr char kIncidentCanceledEventMetatable[] = "sunrise.mission.event.incident_canceled";
inline constexpr char kIncidentRefusedEventMetatable[] = "sunrise.mission.event.incident_refused";
inline constexpr char kScriptableOverrideCommittedEventMetatable[] =
    "sunrise.mission.event.scriptable_override_committed";
// Scriptable override has reached transport.
inline constexpr char kScriptableOverrideTransportStagedEventMetatable[] =
    "sunrise.mission.event.scriptable_override_transport_staged";
// Scriptable override was dropped before transport.
inline constexpr char kScriptableOverrideCanceledEventMetatable[] =
    "sunrise.mission.event.scriptable_override_canceled";
// Operator refusal, then the world changes a program subscribes to.
inline constexpr char kOperatorRefusedEventMetatable[] = "sunrise.mission.event.operator_refused";
inline constexpr char kEffectResultEventMetatable[] = "sunrise.mission.event.effect_result";
inline constexpr char kTriggerEnteredEventMetatable[] = "sunrise.mission.event.trigger_entered";
inline constexpr char kTriggerExitedEventMetatable[] = "sunrise.mission.event.trigger_exited";
inline constexpr char kSquadStateEventMetatable[] = "sunrise.mission.event.squad_state";
inline constexpr char kSquadProvokedEventMetatable[] = "sunrise.mission.event.squad_provoked";
inline constexpr char kEntitySpawnedEventMetatable[] = "sunrise.mission.event.entity_spawned";
inline constexpr char kEntityDiedEventMetatable[] = "sunrise.mission.event.entity_died";
inline constexpr char kSceneFinishedEventMetatable[] = "sunrise.mission.event.scene_finished";
inline constexpr char kObjectiveProgressEventMetatable[] =
    "sunrise.mission.event.objective_progress";
// Program-driven progress: phase, timer, and the client's slot request.
inline constexpr char kPhaseEnteredEventMetatable[] = "sunrise.mission.event.phase_entered";
inline constexpr char kTimerElapsedEventMetatable[] = "sunrise.mission.event.timer_elapsed";
inline constexpr char kEntitySlotsRequestedEventMetatable[] =
    "sunrise.mission.event.entity_slots_requested";
// Session membership, player trigger, and cinematic transitions.
inline constexpr char kSessionJoinedEventMetatable[] = "sunrise.mission.event.session_joined";
inline constexpr char kSessionLeftEventMetatable[] = "sunrise.mission.event.session_left";
inline constexpr char kActorPathEventMetatable[] = "sunrise.mission.event.actor_path";
inline constexpr char kDeviceStateEventMetatable[] = "sunrise.mission.event.device_state";
inline constexpr char kObjectInteractionEventMetatable[] =
    "sunrise.mission.event.object_interaction";
inline constexpr char kGhostLinkEventMetatable[] = "sunrise.mission.event.ghost_link";
inline constexpr char kFireteamStateEventMetatable[] = "sunrise.mission.event.fireteam_state";
inline constexpr char kPlayerTriggerEventMetatable[] = "sunrise.mission.event.player_trigger";
inline constexpr char kCinematicStartedEventMetatable[] = "sunrise.mission.event.cinematic_started";
inline constexpr char kCinematicTerminatedEventMetatable[] =
    "sunrise.mission.event.cinematic_terminated";

/** Mission ABI id of the named device-transition intent. It reuses the device channel route. */
inline constexpr std::string_view kDeviceTransitionSurface = "mission.intent.device-transition.v1";
/** Mission ABI id of the occupancy-condition intent. It reuses the generic slot Auth route. */
inline constexpr std::string_view kOccupancyConditionSurface =
    "mission.intent.occupancy-condition-set.v1";

// Metatable names for one row of each catalog collection above.
inline constexpr char kSquadMetatable[] = "sunrise.sdk.squad";
inline constexpr char kSceneMetatable[] = "sunrise.sdk.authored_scene";
inline constexpr char kSlotMetatable[] = "sunrise.sdk.slot";
inline constexpr char kMessageMetatable[] = "sunrise.sdk.activity_message";
inline constexpr char kMessageFieldMetatable[] = "sunrise.sdk.activity_message_field";

} // namespace sunrise::server::activity::mission::lua_vm::detail
