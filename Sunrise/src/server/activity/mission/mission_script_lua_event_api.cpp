#include <string_view>

#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Resolves a typed event directly from its native enum. */
[[nodiscard]] std::string_view event_kind_name(lua_State* state, host::EventKind kind) noexcept {
    (void)state;
    switch (kind) {
    case host::EventKind::fireteamState:
        return "fireteamState";
    case host::EventKind::senseUpdate:
        return "sensorSenseUpdated";
    case host::EventKind::clientStateChanged:
        return "clientStateChanged";
    case host::EventKind::incidentReceived:
        return "incidentReceived";
    case host::EventKind::timerElapsed:
        return "timerElapsed";
    case host::EventKind::effectResult:
        return "effectResult";
    case host::EventKind::phaseEntered:
        return "phaseEntered";
    case host::EventKind::triggerEntered:
        return "triggerEntered";
    case host::EventKind::triggerExited:
        return "triggerExited";
    case host::EventKind::squadState:
        return "squadState";
    case host::EventKind::squadProvoked:
        return "squadProvoked";
    case host::EventKind::entitySpawned:
        return "entitySpawned";
    case host::EventKind::entityDied:
        return "entityDied";
    case host::EventKind::sceneFinished:
        return "sceneFinished";
    case host::EventKind::objectiveProgress:
        return "objectiveProgress";
    case host::EventKind::entitySlotsRequested:
        return "entitySlotsRequested";
    case host::EventKind::sessionJoined:
        return "sessionJoined";
    case host::EventKind::sessionLeft:
        return "sessionLeft";
    case host::EventKind::playerTrigger:
        return "playerTrigger";
    case host::EventKind::cinematicStarted:
        return "cinematicStarted";
    case host::EventKind::cinematicSkipRequested:
        return "cinematicSkipRequested";
    case host::EventKind::cinematicTerminated:
        return "cinematicTerminated";
    case host::EventKind::actorPathState:
        return "actorPathState";
    case host::EventKind::objectInteracted:
        return "objectInteracted";
    case host::EventKind::damageState:
        return "damageState";
    case host::EventKind::deviceState:
        return "deviceState";
    case host::EventKind::objectState:
        return "objectState";
    case host::EventKind::ghostLinkState:
        return "ghostLinkState";
    case host::EventKind::clientMessageReceived:
        return "clientMessageReceived";
    case host::EventKind::authStateCommitted:
        return "authStateCommitted";
    case host::EventKind::authStateTransportStaged:
        return "authStateTransportStaged";
    case host::EventKind::authStateCanceled:
        return "authStateCanceled";
    case host::EventKind::incidentQueued:
        return "incidentQueued";
    case host::EventKind::incidentTransportStaged:
        return "incidentTransportStaged";
    case host::EventKind::incidentCanceled:
        return "incidentCanceled";
    case host::EventKind::incidentRefused:
        return "incidentRefused";
    case host::EventKind::scriptableOverrideCommitted:
        return "scriptableOverrideCommitted";
    case host::EventKind::scriptableOverrideTransportStaged:
        return "scriptableOverrideTransportStaged";
    case host::EventKind::scriptableOverrideCanceled:
        return "scriptableOverrideCanceled";
    case host::EventKind::operatorRefused:
        return "operatorRefused";
    }
    return "unknown";
}

[[nodiscard]] bool event_surface_visible(lua_State* state, host::EventKind kind) noexcept {
    return !event_kind_name(state, kind).empty();
}

/** @return The view metatable one kind is pushed with. */
[[nodiscard]] const char* event_metatable(host::EventKind kind) noexcept {
    switch (kind) {
    case host::EventKind::fireteamState:
        return kFireteamStateEventMetatable;
    case host::EventKind::senseUpdate:
        return kSenseUpdateEventMetatable;
    case host::EventKind::clientStateChanged:
        return kClientStateChangedEventMetatable;
    case host::EventKind::incidentReceived:
        return kIncidentReceivedEventMetatable;
    case host::EventKind::clientMessageReceived:
        return kClientMessageEventMetatable;
    case host::EventKind::authStateCommitted:
        return kAuthStateCommittedEventMetatable;
    case host::EventKind::authStateTransportStaged:
        return kAuthStateTransportStagedEventMetatable;
    case host::EventKind::authStateCanceled:
        return kAuthStateCanceledEventMetatable;
    case host::EventKind::incidentQueued:
        return kIncidentQueuedEventMetatable;
    case host::EventKind::incidentTransportStaged:
        return kIncidentTransportStagedEventMetatable;
    case host::EventKind::incidentCanceled:
        return kIncidentCanceledEventMetatable;
    case host::EventKind::incidentRefused:
        return kIncidentRefusedEventMetatable;
    case host::EventKind::scriptableOverrideCommitted:
        return kScriptableOverrideCommittedEventMetatable;
    case host::EventKind::scriptableOverrideTransportStaged:
        return kScriptableOverrideTransportStagedEventMetatable;
    case host::EventKind::scriptableOverrideCanceled:
        return kScriptableOverrideCanceledEventMetatable;
    case host::EventKind::operatorRefused:
        return kOperatorRefusedEventMetatable;
    case host::EventKind::effectResult:
        return kEffectResultEventMetatable;
    case host::EventKind::triggerEntered:
        return kTriggerEnteredEventMetatable;
    case host::EventKind::triggerExited:
        return kTriggerExitedEventMetatable;
    case host::EventKind::squadState:
        return kSquadStateEventMetatable;
    case host::EventKind::squadProvoked:
        return kSquadProvokedEventMetatable;
    case host::EventKind::entitySpawned:
        return kEntitySpawnedEventMetatable;
    case host::EventKind::entityDied:
        return kEntityDiedEventMetatable;
    case host::EventKind::sceneFinished:
        return kSceneFinishedEventMetatable;
    case host::EventKind::objectiveProgress:
        return kObjectiveProgressEventMetatable;
    case host::EventKind::phaseEntered:
        return kPhaseEnteredEventMetatable;
    case host::EventKind::timerElapsed:
        return kTimerElapsedEventMetatable;
    case host::EventKind::entitySlotsRequested:
        return kEntitySlotsRequestedEventMetatable;
    case host::EventKind::sessionJoined:
        return kSessionJoinedEventMetatable;
    case host::EventKind::sessionLeft:
        return kSessionLeftEventMetatable;
    case host::EventKind::playerTrigger:
        return kPlayerTriggerEventMetatable;
    case host::EventKind::cinematicStarted:
        return kCinematicStartedEventMetatable;
    case host::EventKind::cinematicSkipRequested:
    case host::EventKind::cinematicTerminated:
        return kCinematicTerminatedEventMetatable;
    case host::EventKind::actorPathState:
        return kActorPathEventMetatable;
    case host::EventKind::objectInteracted:
    case host::EventKind::damageState:
    case host::EventKind::objectState:
        return kObjectInteractionEventMetatable;
    case host::EventKind::deviceState:
        return kDeviceStateEventMetatable;
    case host::EventKind::ghostLinkState:
        return kGhostLinkEventMetatable;
    }
    return kSenseUpdateEventMetatable;
}

/**
 * Reads one event argument of any view.
 * @param index Stack index of the argument.
 * @return The event. The argument's metatable name must carry the event view prefix.
 */
[[nodiscard]] const host::Event& check_event(lua_State* state, int index) {
    if (lua_type(state, index) == LUA_TUSERDATA && lua_getmetatable(state, index) != 0) {
        const bool named = lua_getfield(state, -1, "__name") == LUA_TSTRING;
        const bool view = named && lua_string_view(state, -1).starts_with(kEventMetatablePrefix);
        lua_pop(state, 2);
        if (view) {
            return static_cast<const EventHandle*>(lua_touserdata(state, index))->value;
        }
    }
    raise_lua_error(state, "mission event expected");
}

/**
 * Pushes kind, sequence or source_generation.
 * @param key Member the script asked for.
 * @return True when this helper owns that key.
 */
bool push_common_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "kind") {
        lua_pushinteger(state, static_cast<lua_Integer>(event.kind));
        return true;
    }
    if (key == "sequence") {
        push_u64_string(state, event.sequence);
        return true;
    }
    if (key == "source_generation") {
        push_u64_string(state, event.sourceGeneration);
        return true;
    }
    return false;
}

bool push_mission_sequence_member(lua_State* state,
                                  const host::Event& event,
                                  std::string_view key) {
    if (key != "mission_sequence") {
        return false;
    }
    push_u64_string(state, event.missionSequence);
    return true;
}

bool push_state_revision_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "state_revision") {
        return false;
    }
    push_u64_string(state, event.stateRevision);
    return true;
}

/**
 * Pushes the identity of the one slot an event was observed on.
 * @param key Member the script asked for.
 * @return True when this helper owns that key.
 */
bool push_slot_identity_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "registry_key") {
        lua_pushinteger(state, event.firstRegistryKey);
    } else if (key == "object_tag") {
        lua_pushinteger(state, event.slotObjectTag);
    } else if (key == "slot_index") {
        lua_pushinteger(state, event.firstSlotIndex);
    } else if (key == "slot_type") {
        lua_pushinteger(state, event.firstSlotType);
    } else if (key == "slot") {
        Impl* const impl = impl_from_state(state);
        SlotDefinition definition{};
        const host::SenseObservationKey identity{.registryKey = event.firstRegistryKey,
                                                 .objectTag = event.slotObjectTag,
                                                 .senseSchema = event.slotSenseSchema,
                                                 .slotIndex = event.firstSlotIndex,
                                                 .slotType = event.firstSlotType};
        if (impl == nullptr || impl->definitions.resolveSenseSlot == nullptr
            || !impl->definitions.resolveSenseSlot(
                impl->definitions.context, identity, definition)) {
            lua_pushnil(state);
        } else {
            push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
        }
    } else {
        return false;
    }
    return true;
}

/**
 * Pushes one of the four bounded outer counts of a msg-19 body.
 * @param key Member the script asked for.
 * @return True when this helper owns that key.
 */
bool push_incident_body_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "incident_target") {
        lua_pushinteger(state, event.incidentTarget);
    } else if (key == "incident_extra_targets") {
        lua_pushinteger(state, event.incidentExtraTargets);
    } else if (key == "incident_selector_bytes") {
        lua_pushinteger(state, event.incidentSelectorBytes);
    } else if (key == "incident_payload_bytes") {
        lua_pushinteger(state, event.incidentPayloadBytes);
    } else {
        return false;
    }
    return true;
}

void register_event_metatables(lua_State* state) {
    register_input_event_metatables(state);
    register_delivery_event_metatables(state);
    register_derived_event_metatables(state);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
