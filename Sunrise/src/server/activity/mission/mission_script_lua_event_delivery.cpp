#include <cstdint>
#include <string_view>

#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"

// The twelve delivery lifecycle views. Each carries only the revision arm its producer writes.
// The four incident kinds write union 2's incident arm, so they expose no mission_sequence.

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** scriptable_revision, union 1's scriptable arm. */
[[nodiscard]] bool
push_scriptable_revision_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "scriptable_revision") {
        return false;
    }
    push_u64_string(state, event.scriptableRevision);
    return true;
}

/** incident_revision, union 2's incident arm. */
[[nodiscard]] bool
push_incident_revision_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "incident_revision") {
        return false;
    }
    push_u64_string(state, event.incidentRevision);
    return true;
}

/** @return The direct Lua spelling of one native mission action. */
[[nodiscard]] std::string_view effect_action_name(lua_State* state, std::uint8_t action) noexcept {
    (void)state;
    switch (static_cast<ActionKind>(action)) {
    case ActionKind::placeSquad:
        return "squad.place";
    case ActionKind::activateAuthoredScene:
        return "scene.activate";
    case ActionKind::stopAuthoredScene:
        return "scene.stop";
    case ActionKind::signalAuthoredScene:
        return "scene.send_event";
    case ActionKind::setObjectActive:
        return "slot.set_object_active";
    case ActionKind::setDeviceChannel:
        return "slot.set_channel";
    case ActionKind::applySlotAuth:
        return {};
    case ActionKind::setLifetime:
        return "lifetime.set";
    case ActionKind::restartCheckpoint:
        return "mission.restart_checkpoint";
    case ActionKind::fireTrigger:
        return "slot.fire_trigger";
    case ActionKind::playSequence:
    case ActionKind::playActorSequence:
        return "slot.play_sequence";
    case ActionKind::setCinematicActive:
        return "slot.set_cinematic_active";
    case ActionKind::playPerformance:
        return "slot.play_performance";
    case ActionKind::resetObjectives:
        return "slot.reset_objectives";
    case ActionKind::advanceTask:
        return "slot.advance_task";
    case ActionKind::playDialogueCue:
        return "slot.play_dialogue_cue";
    case ActionKind::selectMissionState:
        return "mission.select_state";
    }
    return {};
}

/** Maps one terminal effect outcome to its Lua spelling. */
[[nodiscard]] std::string_view effect_outcome_name(host::EffectOutcome outcome) noexcept {
    using Outcome = host::EffectOutcome;
    switch (outcome) {
    case Outcome::transportStaged:
        return "transport_staged";
    case Outcome::refused:
        return "refused";
    case Outcome::expired:
        return "expired";
    case Outcome::canceled:
        return "canceled";
    }
    return {};
}

/** The four members an effect result carries beyond the common set and mission_sequence. */
[[nodiscard]] bool
push_effect_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "request_key") {
        push_request_key(state, event.effectRequestKey);
    } else if (key == "effect") {
        const std::string_view name = effect_action_name(state, event.effectAction);
        if (name.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, name.data(), name.size());
        }
    } else if (key == "outcome") {
        const std::string_view outcome = effect_outcome_name(event.effectOutcome);
        if (outcome.empty()) {
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, outcome.data(), outcome.size());
        }
    } else if (key == "outcome_code") {
        lua_pushinteger(state, static_cast<lua_Integer>(event.effectOutcome));
    } else {
        return false;
    }
    return true;
}

/** Reads one member of a committed Auth state delivery edge. */
[[nodiscard]] int auth_state_committed_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_state_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of an Auth state delivery edge that reached the transport queue. */
[[nodiscard]] int auth_state_transport_staged_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_state_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a canceled Auth state delivery edge. */
[[nodiscard]] int auth_state_canceled_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_state_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a queued incident delivery edge. */
[[nodiscard]] int incident_queued_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_incident_revision_member(state, event, key)
        || push_incident_body_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of an incident delivery edge that reached the transport queue. */
[[nodiscard]] int incident_transport_staged_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_incident_revision_member(state, event, key)
        || push_incident_body_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a canceled incident delivery edge. Body members are 0 with no record. */
[[nodiscard]] int incident_canceled_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_incident_revision_member(state, event, key)
        || push_incident_body_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a refused incident delivery edge. incident_revision is always 0 here. */
[[nodiscard]] int incident_refused_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_incident_revision_member(state, event, key)
        || push_incident_body_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a committed scriptable override delivery edge. */
[[nodiscard]] int scriptable_override_committed_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key)
        || push_scriptable_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a scriptable override edge that reached the transport queue. */
[[nodiscard]] int scriptable_override_transport_staged_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key)
        || push_scriptable_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a canceled scriptable override delivery edge. */
[[nodiscard]] int scriptable_override_canceled_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key)
        || push_scriptable_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a refused operator output. state_revision is 0 on most refusal paths. */
[[nodiscard]] int operator_refused_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_state_revision_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a script effect's terminal delivery result. */
[[nodiscard]] int effect_result_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind) && push_effect_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

} // namespace

/** Installs the twelve delivery lifecycle view metatables. */
void register_delivery_event_metatables(lua_State* state) {
    register_metatable(state, kAuthStateCommittedEventMetatable, &auth_state_committed_index);
    register_metatable(
        state, kAuthStateTransportStagedEventMetatable, &auth_state_transport_staged_index);
    register_metatable(state, kAuthStateCanceledEventMetatable, &auth_state_canceled_index);
    register_metatable(state, kIncidentQueuedEventMetatable, &incident_queued_index);
    register_metatable(
        state, kIncidentTransportStagedEventMetatable, &incident_transport_staged_index);
    register_metatable(state, kIncidentCanceledEventMetatable, &incident_canceled_index);
    register_metatable(state, kIncidentRefusedEventMetatable, &incident_refused_index);
    register_metatable(
        state, kScriptableOverrideCommittedEventMetatable, &scriptable_override_committed_index);
    register_metatable(state,
                       kScriptableOverrideTransportStagedEventMetatable,
                       &scriptable_override_transport_staged_index);
    register_metatable(
        state, kScriptableOverrideCanceledEventMetatable, &scriptable_override_canceled_index);
    register_metatable(state, kOperatorRefusedEventMetatable, &operator_refused_index);
    register_metatable(state, kEffectResultEventMetatable, &effect_result_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
