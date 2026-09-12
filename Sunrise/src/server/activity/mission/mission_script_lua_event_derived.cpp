#include <array>
#include <cstddef>
#include <string_view>

#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"

// The derived and internal event views. Every one carries mission_sequence. The Sense
// edges also carry the slot identity; the phase, timer, world and session edges leave those fields
// zero, so they must not expose them.

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** The type-60 target read out of one type-31 player-trigger incident. */
[[nodiscard]] bool
push_player_trigger_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "volume_registry_key") {
        lua_pushinteger(state, event.playerTriggerRegistryKey);
    } else if (key == "volume_slot_type") {
        lua_pushinteger(state, event.playerTriggerSlotType);
    } else if (key == "volume_slot_index") {
        lua_pushinteger(state, event.playerTriggerSlotIndex);
    } else if (key == "resolved_object_id") {
        lua_pushinteger(state, event.playerTriggerResolvedObjectId);
    } else {
        return false;
    }
    return true;
}

/** Runtime values carried beside the exact Type-6 source ClientRef. */
[[nodiscard]] bool
push_cinematic_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "runtime_object_id") {
        push_u64_string(state, event.cinematicRuntimeObjectId);
    } else if (key == "event_value") {
        lua_pushnumber(state, event.cinematicEventValue);
    } else {
        return false;
    }
    return true;
}

/** The three members every trigger volume occupancy edge carries. */
[[nodiscard]] bool
push_trigger_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "member_count") {
        lua_pushinteger(state, event.triggerCount);
    } else if (key == "value") {
        lua_pushinteger(state, event.triggerValue);
    } else if (key == "all_inside") {
        lua_pushboolean(state, event.triggerAll ? 1 : 0);
    } else {
        return false;
    }
    return true;
}

/** Squad totals plus the per-slot count list, pushed as a one-based Lua array. */
[[nodiscard]] bool
push_squad_state_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "alive_count") {
        lua_pushinteger(state, event.squadAliveCount);
    } else if (key == "objective_revision") {
        lua_pushinteger(state, event.squadObjectiveRevision);
    } else if (key == "task_costs") {
        lua_createtable(state, static_cast<int>(host::kSquadObjectiveGroupCount), 0);
        for (unsigned group = 0; group < host::kSquadObjectiveGroupCount; ++group) {
            if ((event.squadObjectiveCostMask & (1U << group)) == 0) {
                continue;
            }
            lua_pushnumber(state, event.squadObjectiveCosts[group]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(group) + 1);
        }
    } else if (key == "previous_alive_count") {
        lua_pushinteger(state, event.squadPreviousAliveCount);
    } else if (key == "removal_flag") {
        lua_pushboolean(state, event.squadRemovalFlag ? 1 : 0);
    } else if (key == "slot_counts") {
        const std::size_t count = event.squadSlotCountLength;
        lua_createtable(state, static_cast<int>(count), 0);
        for (std::size_t index = 0; index < count; ++index) {
            lua_pushinteger(state, event.squadSlotCounts[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
    } else {
        return false;
    }
    return true;
}

/** The one squad slot that rose, and the count it held before. */
[[nodiscard]] bool
push_entity_spawned_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "member_slot") {
        lua_pushinteger(state, event.squadSlotOrdinal);
    } else if (key == "count") {
        lua_pushinteger(state, event.squadSlotValue);
    } else if (key == "previous_count") {
        lua_pushinteger(state, event.squadPreviousSlotValue);
    } else {
        return false;
    }
    return true;
}

/** The alive count after the death, and the count it replaced. */
[[nodiscard]] bool
push_entity_died_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "alive_count") {
        lua_pushinteger(state, event.squadAliveCount);
    } else if (key == "previous_alive_count") {
        lua_pushinteger(state, event.squadPreviousAliveCount);
    } else {
        return false;
    }
    return true;
}

/** The token the completed authored scene echoed back. */
[[nodiscard]] bool
push_scene_finished_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "activation_token") {
        return false;
    }
    lua_pushinteger(state, event.sceneActivationToken);
    return true;
}

/** The objective and task ordinals with the counter that rose. */
[[nodiscard]] bool
push_objective_progress_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "objective") {
        lua_pushinteger(state, event.objectiveOrdinal);
    } else if (key == "task") {
        lua_pushinteger(state, event.objectiveTaskOrdinal);
    } else if (key == "task_count") {
        lua_pushinteger(state, event.objectiveTaskCount);
    } else if (key == "previous_task_count") {
        lua_pushinteger(state, event.objectivePreviousTaskCount);
    } else {
        return false;
    }
    return true;
}

/** The committed mission phase and the phase it replaced. */
[[nodiscard]] bool
push_phase_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "phase") {
        lua_pushinteger(state, event.missionPhase);
    } else if (key == "previous_phase") {
        lua_pushinteger(state, event.previousMissionPhase);
    } else {
        return false;
    }
    return true;
}

/** Timer identity and its armed deadline. The name is a bounded buffer with its own length. */
[[nodiscard]] bool
push_timer_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "timer_name") {
        lua_pushlstring(state, event.timerName.bytes.data(), event.timerName.length);
    } else if (key == "timer_deadline_tick") {
        push_u64_string(state, event.timerDeadlineTick);
    } else if (key == "timer_sequence") {
        push_u64_string(state, event.timerSequence);
    } else {
        return false;
    }
    return true;
}

/** The peer identity both session edges carry. */
[[nodiscard]] bool
push_session_peer_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key == "session_id") {
        push_u64_string(state, event.peerSessionId);
    } else if (key == "session_generation") {
        push_u64_string(state, event.peerSessionGeneration);
    } else if (key == "member_key") {
        push_u64_string(state, event.peerMemberKey);
    } else {
        return false;
    }
    return true;
}

/** Mission state revision at the join. Only the join edge writes union 1's state arm. */
[[nodiscard]] bool
push_joined_revision_member(lua_State* state, const host::Event& event, std::string_view key) {
    if (key != "joined_revision") {
        return false;
    }
    push_u64_string(state, event.stateRevision);
    return true;
}

/** Reads one member of a trigger volume entry edge. */
[[nodiscard]] int trigger_entered_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_trigger_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a trigger volume exit edge. */
[[nodiscard]] int trigger_exited_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_trigger_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one native player-trigger notification resolved to its authored type-31 source. */
[[nodiscard]] int player_trigger_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_player_trigger_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one exact authored cinematic start edge. */
[[nodiscard]] int cinematic_started_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_cinematic_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one exact authored cinematic terminal edge. */
[[nodiscard]] int cinematic_terminated_index(lua_State* state) {
    return cinematic_started_index(state);
}

/** Reports the accepted member generation and native movement-path state. */
[[nodiscard]] int actor_path_index(lua_State* state) {
    const auto& event = check_event(state, 1);
    const auto key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)) {
        if (push_slot_identity_member(state, event, key)) {
            return 1;
        }
        if (key == "generation") {
            lua_pushinteger(state, event.actorGeneration);
            return 1;
        }
        if (key == "revision") {
            lua_pushinteger(state, event.actorPathRevision);
            return 1;
        }
        if (key == "path_state") {
            lua_pushinteger(state, event.actorPathState);
            return 1;
        }
        if (key == "delivery_revision" && event.actorDeliveryKnown) {
            lua_pushinteger(state, event.actorDeliveryRevision);
            return 1;
        }
        if (key == "delivery_state" && event.actorDeliveryKnown) {
            lua_pushinteger(state, event.actorDeliveryState);
            return 1;
        }
        if (key == "dead") {
            lua_pushboolean(state, event.actorDead);
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/**
 * Exposes reported device values without giving unknown fields a numeric default.
 * @param state Lua call holding the device event and requested member name.
 * @return One reported value, flag or nil.
 */
[[nodiscard]] int device_state_index(lua_State* state) {
    const auto& event = check_event(state, 1);
    const auto key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || push_slot_identity_member(state, event, key)) {
        return 1;
    }
    static constexpr std::array<std::string_view, 3> values{"position", "power", "lock"};
    static constexpr std::array<std::string_view, 3> sequences{
        "position_sequence", "power_sequence", "lock_sequence"};
    for (std::size_t channel = 0; channel < values.size(); ++channel) {
        if (key == values[channel]) {
            if ((event.deviceValueMask & (1U << channel)) != 0) {
                lua_pushnumber(state, event.deviceValues[channel]);
            } else {
                lua_pushnil(state);
            }
            return 1;
        }
        if (key == sequences[channel]) {
            if ((event.deviceSequenceMask & (1U << channel)) != 0) {
                lua_pushinteger(state, event.deviceSequences[channel]);
            } else {
                lua_pushnil(state);
            }
            return 1;
        }
    }
    if (key == "first_report") {
        lua_pushboolean(state, event.deviceFirstReport);
    } else if (key == "reset") {
        lua_pushboolean(state, event.deviceReset);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for one object interaction event: the common, slot and interaction members. */
[[nodiscard]] int object_interaction_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const auto key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)
        || (event_surface_visible(state, event.kind)
            && push_slot_identity_member(state, event, key))) {
        return 1;
    }
    if (event.kind == host::EventKind::damageState) {
        if (key == "health") {
            lua_pushnumber(state, event.damageHealth);
        } else if (key == "shield") {
            lua_pushnumber(state, event.damageShield);
        } else if (key == "revision") {
            lua_pushinteger(state, event.damageRevision);
        } else {
            lua_pushnil(state);
        }
        return 1;
    }
    if (key == "generation") {
        lua_pushinteger(state, event.objectGeneration);
    } else if (key == "present") {
        lua_pushboolean(state, event.objectPresent);
    } else if (key == "alive") {
        lua_pushboolean(state, event.objectAlive);
    } else if (key == "owner_known") {
        lua_pushboolean(state, event.objectOwnerKnown);
    } else if (key == "has_owner") {
        lua_pushboolean(state, event.objectHasOwner);
    } else if (key == "owner_key" && event.objectOwnerKey != 0) {
        push_u64_string(state, event.objectOwnerKey);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Reports native interaction progress without exposing raw Sense storage. */
[[nodiscard]] int ghost_link_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)) {
        if (push_slot_identity_member(state, event, key)) {
            return 1;
        }
        if (key == "generation") {
            lua_pushinteger(state, event.ghostGeneration);
            return 1;
        }
        if (key == "progress") {
            lua_pushnumber(state, event.ghostProgress);
            return 1;
        }
        if (key == "active") {
            lua_pushboolean(state, event.ghostActive);
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a squad occupancy edge. */
[[nodiscard]] int squad_state_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_squad_state_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** A gameplay provocation carries the exact squad reference, not a native Sense body. */
[[nodiscard]] int squad_provoked_index(lua_State* state) {
    const auto& event = check_event(state, 1);
    const auto key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (key == "registry_key") {
        lua_pushinteger(state, event.firstRegistryKey);
    } else if (key == "slot_type") {
        lua_pushinteger(state, event.firstSlotType);
    } else if (key == "slot_index") {
        lua_pushinteger(state, event.firstSlotIndex);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Reads one member of a squad member spawn edge. */
[[nodiscard]] int entity_spawned_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_entity_spawned_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a squad member death edge. */
[[nodiscard]] int entity_died_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_entity_died_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of an authored scene completion edge. */
[[nodiscard]] int scene_finished_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_scene_finished_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of an objective task progress edge. */
[[nodiscard]] int objective_progress_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_slot_identity_member(state, event, key)
            || push_objective_progress_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a mission phase commit edge. */
[[nodiscard]] int phase_entered_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_phase_member(state, event, key)
            || push_state_revision_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a mission timer expiry edge. */
[[nodiscard]] int timer_elapsed_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind) && push_timer_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads the exact requested count from one committed msg-20 edge. */
[[nodiscard]] int entity_slots_requested_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind) && key == "requested_count") {
        lua_pushinteger(state, event.requestedEntitySlots);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a peer session join edge. */
[[nodiscard]] int session_joined_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind)
        && (push_session_peer_member(state, event, key)
            || push_joined_revision_member(state, event, key))) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Reads one member of a peer session leave edge. */
[[nodiscard]] int session_left_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (event_surface_visible(state, event.kind) && push_session_peer_member(state, event, key)) {
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

/** Lua index for one fireteam state event: the alive, dead and unknown counts. */
[[nodiscard]] int fireteam_state_index(lua_State* state) {
    const auto& event = check_event(state, 1);
    const auto key = lua_string_view(state, 2);
    if (push_common_member(state, event, key) || push_mission_sequence_member(state, event, key)) {
        return 1;
    }
    if (key == "alive_count") {
        lua_pushinteger(state, event.fireteamAlive);
    } else if (key == "dead_count") {
        lua_pushinteger(state, event.fireteamDead);
    } else if (key == "unknown_count") {
        lua_pushinteger(state, event.fireteamUnknown);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

} // namespace

/** Installs the derived and internal view metatables. */
void register_derived_event_metatables(lua_State* state) {
    register_metatable(state, kFireteamStateEventMetatable, &fireteam_state_index);
    register_metatable(state, kTriggerEnteredEventMetatable, &trigger_entered_index);
    register_metatable(state, kTriggerExitedEventMetatable, &trigger_exited_index);
    register_metatable(state, kSquadStateEventMetatable, &squad_state_index);
    register_metatable(state, kSquadProvokedEventMetatable, &squad_provoked_index);
    register_metatable(state, kEntitySpawnedEventMetatable, &entity_spawned_index);
    register_metatable(state, kEntityDiedEventMetatable, &entity_died_index);
    register_metatable(state, kSceneFinishedEventMetatable, &scene_finished_index);
    register_metatable(state, kObjectiveProgressEventMetatable, &objective_progress_index);
    register_metatable(state, kPhaseEnteredEventMetatable, &phase_entered_index);
    register_metatable(state, kTimerElapsedEventMetatable, &timer_elapsed_index);
    register_metatable(state, kEntitySlotsRequestedEventMetatable, &entity_slots_requested_index);
    register_metatable(state, kSessionJoinedEventMetatable, &session_joined_index);
    register_metatable(state, kSessionLeftEventMetatable, &session_left_index);
    register_metatable(state, kPlayerTriggerEventMetatable, &player_trigger_index);
    register_metatable(state, kCinematicStartedEventMetatable, &cinematic_started_index);
    register_metatable(state, kCinematicTerminatedEventMetatable, &cinematic_terminated_index);
    register_metatable(state, kGhostLinkEventMetatable, &ghost_link_index);
    register_metatable(state, kObjectInteractionEventMetatable, &object_interaction_index);
    register_metatable(state, kDeviceStateEventMetatable, &device_state_index);
    register_metatable(state, kActorPathEventMetatable, &actor_path_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
