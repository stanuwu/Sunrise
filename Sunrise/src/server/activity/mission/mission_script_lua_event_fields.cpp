// Event views. Every view serves the field groups the table lists for its kind; a group binds its
// fields in table order, so a field the table does not declare is never reachable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"
#include "mission_trigger_observation.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

using G = api::FieldGroupId;
using EventBinding = Binding<host::Event>;
using EventBindings = std::span<const EventBinding>;

/** Pushes one integer field of the event. */
template <auto Field> void integer_field(lua_State* state, const host::Event& event) {
    lua_pushinteger(state, static_cast<lua_Integer>(event.*Field));
}

/** Pushes one 64-bit field of the event as a decimal string. */
template <auto Field> void decimal_field(lua_State* state, const host::Event& event) {
    push_u64_string(state, event.*Field);
}

/** Pushes one real field of the event. */
template <auto Field> void number_field(lua_State* state, const host::Event& event) {
    lua_pushnumber(state, event.*Field);
}

/** Pushes one flag of the event. */
template <auto Field> void boolean_field(lua_State* state, const host::Event& event) {
    lua_pushboolean(state, event.*Field ? 1 : 0);
}

/** Pushes the slot the event's identity resolves to, or nil outside this activity. */
void push_event_slot(lua_State* state, const host::Event& event) {
    Impl* const impl = impl_from_state(state);
    SlotDefinition definition{};
    const host::SenseObservationKey identity{.registryKey = event.firstRegistryKey,
                                             .objectTag = event.slotObjectTag,
                                             .senseSchema = event.slotSenseSchema,
                                             .slotIndex = event.firstSlotIndex,
                                             .slotType = event.firstSlotType};
    if (impl == nullptr || impl->definitions.resolveSenseSlot == nullptr
        || !impl->definitions.resolveSenseSlot(impl->definitions.context, identity, definition)) {
        lua_pushnil(state);
    } else {
        push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    }
}

/** Pushes one slot identity field only when the update named an object. */
template <void (*Push)(lua_State*, const host::Event&)>
void first_object_field(lua_State* state, const host::Event& event) {
    if (event.hasFirstObject) {
        Push(state, event);
    } else {
        lua_pushnil(state);
    }
}

/** Pushes one flag only when its presence flag is set. */
template <auto Field, auto Present>
void present_boolean(lua_State* state, const host::Event& event) {
    if (event.*Present) {
        lua_pushboolean(state, event.*Field ? 1 : 0);
    } else {
        lua_pushnil(state);
    }
}

/** Pushes one integer field only when its presence flag is set. */
template <auto Field, auto Present>
void present_integer(lua_State* state, const host::Event& event) {
    if (event.*Present) {
        lua_pushinteger(state, static_cast<lua_Integer>(event.*Field));
    } else {
        lua_pushnil(state);
    }
}

/** Pushes one catalog view of the event's client message, or nil without a catalog row. */
template <bool Row> void client_message_row(lua_State* state, const host::Event& event) {
    ActivityMessageDefinition definition{};
    if (!resolve_message_id(state, event.clientMessageType, definition)) {
        lua_pushnil(state);
    } else if (Row) {
        push_message(state, definition.localRow);
    } else {
        lua_pushlstring(state, definition.name.data(), definition.name.size());
    }
}

/** @return The direct Lua spelling of one native mission action, or empty for a generic one. */
[[nodiscard]] std::string_view effect_action_name(std::uint8_t action) noexcept {
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
    case ActionKind::runActorProgram:
        return "slot.run_atoms";
    case ActionKind::retireActor:
        return "slot.retire_actor";
    case ActionKind::setInteractableObject:
        return "slot.set_interactable_object";
    case ActionKind::setGhostLink:
        return "slot.set_ghost_link";
    case ActionKind::watchDamage:
        return "slot.watch_damage";
    case ActionKind::assignCombatObjective:
        return "slot.assign_combat_objective";
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
    case ActionKind::holdSpawn:
        return "mission.hold_spawn";
    case ActionKind::selectMissionState:
        return "mission.select_state";
    case ActionKind::applySlotAuth:
    case ActionKind::bindCombatantToSquad:
    case ActionKind::actorCommand:
        return {};
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

// The table entry of device_state:applied_request.
constexpr const api::MemberSpec& kAppliedRequest =
    api::find_member(api::group_members(G::device), "applied_request");

/**
 * Returns the exact request first satisfied by this channel's accepted report.
 * @param state Lua call holding the event and named channel argument.
 * @return One RequestKey, or nil when this report satisfies no request on the channel.
 */
[[nodiscard]] int device_applied_request(lua_State* state) {
    const auto& event = check_event(state, 1);
    refuse_unknown_arguments(state, kAppliedRequest);
    const auto channel = checked_argument<DeviceChannelHandle>(
        state,
        required_key(kAppliedRequest, "channel", api::ValueKind::enumValue),
        kDeviceChannelMetatable);
    const auto request = event.deviceAppliedRequests[channel.channel];
    if (request != 0) {
        push_request_key(state, request);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Pushes one device channel value, or nil when this report did not carry it. */
template <std::size_t Channel> void device_value(lua_State* state, const host::Event& event) {
    if ((event.deviceValueMask & (1U << Channel)) != 0) {
        lua_pushnumber(state, event.deviceValues[Channel]);
    } else {
        lua_pushnil(state);
    }
}

/** Pushes one device channel sequence, or nil when this report did not carry it. */
template <std::size_t Channel> void device_sequence(lua_State* state, const host::Event& event) {
    if ((event.deviceSequenceMask & (1U << Channel)) != 0) {
        lua_pushinteger(state, event.deviceSequences[Channel]);
    } else {
        lua_pushnil(state);
    }
}

// Device fields follow the native position, power and lock channel order.
constexpr std::size_t kPosition = api::device_channel_value("position");
constexpr std::size_t kPower = api::device_channel_value("power");
constexpr std::size_t kLock = api::device_channel_value("lock");

// --- Group bindings, in table order ------------------------------------------------------------

using E = host::Event;

// Fields every event view carries.
constexpr EventBinding kCommon[]{
    {"kind",
     nullptr,
     [](lua_State* state, const E& event) {
         lua_pushinteger(state, static_cast<lua_Integer>(event.kind));
     }},
    {"sequence", nullptr, &decimal_field<&E::sequence>},
    {"source_generation", nullptr, &decimal_field<&E::sourceGeneration>},
    {"attempt_generation", nullptr, &decimal_field<&E::attemptGeneration>},
};
constexpr EventBinding kMissionSequence[]{
    {"mission_sequence", nullptr, &decimal_field<&E::missionSequence>}};
constexpr EventBinding kStateRevision[]{
    {"state_revision", nullptr, &decimal_field<&E::stateRevision>}};
constexpr EventBinding kSlotIdentity[]{
    {"registry_key", nullptr, &integer_field<&E::firstRegistryKey>},
    {"object_tag", nullptr, &integer_field<&E::slotObjectTag>},
    {"slot_index", nullptr, &integer_field<&E::firstSlotIndex>},
    {"slot_type", nullptr, &integer_field<&E::firstSlotType>},
    {"slot", nullptr, &push_event_slot},
};
// The update names one slot only: the first decoded ClientRef of the packet.
constexpr EventBinding kFirstObject[]{
    {"registry_key", nullptr, &first_object_field<&integer_field<&E::firstRegistryKey>>},
    {"object_tag", nullptr, &first_object_field<&integer_field<&E::slotObjectTag>>},
    {"slot_index", nullptr, &first_object_field<&integer_field<&E::firstSlotIndex>>},
    {"slot_type", nullptr, &first_object_field<&integer_field<&E::firstSlotType>>},
    {"slot", nullptr, &first_object_field<&push_event_slot>},
};
constexpr EventBinding kClientInput[]{
    {"client_message_sequence", nullptr, &decimal_field<&E::clientMessageSequence>},
    {"payload_bytes", nullptr, &integer_field<&E::payloadBytes>},
};
constexpr EventBinding kPeerHeard[]{
    {"peer_heard_mask", nullptr, &integer_field<&E::peerHeardMask>}};
constexpr EventBinding kSenseDecode[]{
    {"objects_decoded", nullptr, &integer_field<&E::objectsDecoded>},
    {"groups_decoded", nullptr, &integer_field<&E::groupsDecoded>},
};
constexpr EventBinding kClientState[]{
    {"activity_state_revision", nullptr, &decimal_field<&E::activityStateRevision>},
    {"membership_revision", nullptr, &integer_field<&E::membershipRevision>},
    {"region_index", nullptr, &present_integer<&E::regionIndex, &E::clientStateHasRegion>},
    {"current_region_index",
     nullptr,
     &present_integer<&E::currentRegionIndex, &E::clientStateHasCurrentRegion>},
    {"held_region_index",
     nullptr,
     [](lua_State* state, const E& event) {
         if (event.heldRegionIndex >= 0) {
             lua_pushinteger(state, event.heldRegionIndex);
         } else {
             lua_pushnil(state);
         }
     }},
    {"region_slice_set_hash",
     nullptr,
     &present_integer<&E::regionSliceSetHash, &E::clientStateHasRegion>},
    {"spawn_state", nullptr, &present_integer<&E::spawnState, &E::clientStateHasSpawn>},
    {"teleport_state", nullptr, &present_integer<&E::teleportState, &E::clientStateHasTeleport>},
    {"teleport_slice_set_index",
     nullptr,
     &present_integer<&E::teleportSliceSetIndex, &E::clientStateHasTeleport>},
    {"teleport_slice_set_hash",
     nullptr,
     &present_integer<&E::teleportSliceSetHash, &E::clientStateHasTeleport>},
    {"entered",
     nullptr,
     [](lua_State* state, const E& event) {
         if (event.clientEntered) {
             lua_pushboolean(state, 1);
         } else {
             lua_pushnil(state);
         }
     }},
};
constexpr EventBinding kIncidentBody[]{
    {"incident_target", nullptr, &integer_field<&E::incidentTarget>},
    {"incident_extra_targets", nullptr, &integer_field<&E::incidentExtraTargets>},
    {"incident_selector_bytes", nullptr, &integer_field<&E::incidentSelectorBytes>},
    {"incident_payload_bytes", nullptr, &integer_field<&E::incidentPayloadBytes>},
};
constexpr EventBinding kClientMessage[]{
    {"message_type", nullptr, &integer_field<&E::clientMessageType>},
    {"message_status",
     nullptr,
     [](lua_State* state, const E& event) {
         lua_pushstring(state, lua_client_message_status_name(event.clientMessageStatus));
     }},
    {"message_name", nullptr, &client_message_row<false>},
    {"message", nullptr, &client_message_row<true>},
};
constexpr EventBinding kScriptableRevision[]{
    {"scriptable_revision", nullptr, &decimal_field<&E::scriptableRevision>}};
constexpr EventBinding kIncidentRevision[]{
    {"incident_revision", nullptr, &decimal_field<&E::incidentRevision>}};
constexpr EventBinding kEffect[]{
    {"request_key",
     nullptr,
     [](lua_State* state, const E& event) { push_request_key(state, event.effectRequestKey); }},
    {"effect",
     nullptr,
     [](lua_State* state, const E& event) {
         push_optional_name(state, effect_action_name(event.effectAction));
     }},
    {"outcome",
     nullptr,
     [](lua_State* state, const E& event) {
         push_optional_name(state, effect_outcome_name(event.effectOutcome));
     }},
    {"outcome_code", nullptr, &integer_field<&E::effectOutcome>},
};
constexpr EventBinding kRegion[]{
    {"region_index", nullptr, &integer_field<&E::regionIndex>},
    {"previous_region_index",
     nullptr,
     [](lua_State* state, const E& event) {
         if (event.previousRegionIndex >= 0) {
             lua_pushinteger(state, event.previousRegionIndex);
         } else {
             lua_pushnil(state);
         }
     }},
};
constexpr EventBinding kPlayerTrigger[]{
    {"volume_registry_key", nullptr, &integer_field<&E::playerTriggerRegistryKey>},
    {"volume_slot_type", nullptr, &integer_field<&E::playerTriggerSlotType>},
    {"volume_slot_index", nullptr, &integer_field<&E::playerTriggerSlotIndex>},
    {"resolved_object_id", nullptr, &integer_field<&E::playerTriggerResolvedObjectId>},
};
constexpr EventBinding kCinematic[]{
    {"runtime_object_id", nullptr, &decimal_field<&E::cinematicRuntimeObjectId>},
    {"event_value", nullptr, &number_field<&E::cinematicEventValue>},
};
constexpr EventBinding kTrigger[]{
    {"member_count", nullptr, &integer_field<&E::triggerCount>},
    {"value", nullptr, &integer_field<&E::triggerValue>},
    {"all_inside", nullptr, &boolean_field<&E::triggerAll>},
};
constexpr EventBinding kSquadAlive[]{
    {"alive_count", nullptr, &integer_field<&E::squadAliveCount>},
    {"previous_alive_count", nullptr, &integer_field<&E::squadPreviousAliveCount>},
};
constexpr EventBinding kSquadState[]{
    {"task_cost", &event_task_cost},
    {"task_group", &event_task_group},
    {"slot_counts",
     nullptr,
     [](lua_State* state, const E& event) {
         const std::size_t count = event.squadSlotCountLength;
         lua_createtable(state, static_cast<int>(count), 0);
         for (std::size_t index = 0; index < count; ++index) {
             lua_pushinteger(state, event.squadSlotCounts[index]);
             lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
         }
     }},
    {"initial_observation", nullptr, &boolean_field<&E::initialObservation>},
    {"population_available", nullptr, &boolean_field<&E::squadPopulationAvailable>},
    {"objective_revision", nullptr, &integer_field<&E::squadObjectiveRevision>},
    {"task_costs",
     nullptr,
     [](lua_State* state, const E& event) {
         lua_createtable(state, static_cast<int>(host::kSquadObjectiveGroupCount), 0);
         for (unsigned group = 0; group < host::kSquadObjectiveGroupCount; ++group) {
             if ((event.squadObjectiveCostMask & (1U << group)) != 0) {
                 lua_pushnumber(state, event.squadObjectiveCosts[group]);
                 lua_rawseti(state, -2, static_cast<lua_Integer>(group) + 1);
             }
         }
     }},
};
constexpr EventBinding kEntitySpawned[]{
    {"member_slot", nullptr, &integer_field<&E::squadSlotOrdinal>},
    {"count", nullptr, &integer_field<&E::squadSlotValue>},
    {"previous_count", nullptr, &integer_field<&E::squadPreviousSlotValue>},
};
constexpr EventBinding kSceneFinished[]{
    {"activation_token", nullptr, &integer_field<&E::sceneActivationToken>}};
constexpr EventBinding kSceneOutput[]{
    {"key", nullptr, &integer_field<&E::sceneOutputKey>},
    {"activation_token", nullptr, &integer_field<&E::sceneActivationToken>},
};
constexpr EventBinding kObjectiveProgress[]{
    {"objective", nullptr, &integer_field<&E::objectiveOrdinal>},
    {"task", nullptr, &integer_field<&E::objectiveTaskOrdinal>},
    {"task_count", nullptr, &integer_field<&E::objectiveTaskCount>},
    {"previous_task_count", nullptr, &integer_field<&E::objectivePreviousTaskCount>},
};
constexpr EventBinding kPhase[]{
    {"phase", nullptr, &integer_field<&E::missionPhase>},
    {"previous_phase", nullptr, &integer_field<&E::previousMissionPhase>},
};
// The timer name is a bounded buffer with its own length.
constexpr EventBinding kTimer[]{
    {"timer_name",
     nullptr,
     [](lua_State* state, const E& event) {
         lua_pushlstring(state, event.timerName.bytes.data(), event.timerName.length);
     }},
    {"timer_deadline_tick", nullptr, &decimal_field<&E::timerDeadlineTick>},
    {"timer_sequence", nullptr, &decimal_field<&E::timerSequence>},
};
constexpr EventBinding kEntitySlots[]{
    {"requested_count", nullptr, &integer_field<&E::requestedEntitySlots>}};
constexpr EventBinding kSessionPeer[]{
    {"session_id", nullptr, &decimal_field<&E::peerSessionId>},
    {"session_generation", nullptr, &decimal_field<&E::peerSessionGeneration>},
    {"member_key", nullptr, &decimal_field<&E::peerMemberKey>},
};
// Only the join edge writes union 1's state arm.
constexpr EventBinding kJoinedRevision[]{
    {"joined_revision", nullptr, &decimal_field<&E::stateRevision>}};
constexpr EventBinding kActorPath[]{
    {"generation", nullptr, &integer_field<&E::actorGeneration>},
    {"revision", nullptr, &integer_field<&E::actorPathRevision>},
    {"path_state", nullptr, &integer_field<&E::actorPathState>},
    {"delivery_revision",
     nullptr,
     &present_integer<&E::actorDeliveryRevision, &E::actorDeliveryKnown>},
    {"delivery_state", nullptr, &present_integer<&E::actorDeliveryState, &E::actorDeliveryKnown>},
    {"suppressed", nullptr, &boolean_field<&E::actorSuppressed>},
};
constexpr EventBinding kDevice[]{
    {"position", nullptr, &device_value<kPosition>},
    {"power", nullptr, &device_value<kPower>},
    {"lock", nullptr, &device_value<kLock>},
    {"position_sequence", nullptr, &device_sequence<kPosition>},
    {"power_sequence", nullptr, &device_sequence<kPower>},
    {"lock_sequence", nullptr, &device_sequence<kLock>},
    {"first_report", nullptr, &boolean_field<&E::deviceFirstReport>},
    {"reset", nullptr, &boolean_field<&E::deviceReset>},
    {"applied_request", &device_applied_request},
};
/** Pushes one channel-2 pool level, or nil unless an entity that reports it dealt the damage. */
template <std::size_t Pool> void entity_pool_level(lua_State* state, const E& event) {
    if (event.damageFromEntity && event.damageEntityPools.present[Pool]) {
        lua_pushinteger(state, event.damageEntityPools.levels[Pool]);
    } else {
        lua_pushnil(state);
    }
}
/** Pushes one integer of the damaging entity's lifetime, or nil when no entity dealt it. */
void entity_lifetime_integer(lua_State* state, const E& event, lua_Integer value) {
    if (event.damageFromEntity) {
        lua_pushinteger(state, value);
    } else {
        lua_pushnil(state);
    }
}
constexpr EventBinding kDamage[]{
    {"health", nullptr, &number_field<&E::damageHealth>},
    {"shield", nullptr, &number_field<&E::damageShield>},
    {"revision", nullptr, &integer_field<&E::damageRevision>},
    {"from_entity", nullptr, &boolean_field<&E::damageFromEntity>},
    {"health_level",
     nullptr,
     &entity_pool_level<state::gameplay::entity_identity::kHealthPoolIndex>},
    {"shield_level",
     nullptr,
     &entity_pool_level<state::gameplay::entity_identity::kShieldPoolIndex>},
    {"entity_slot",
     nullptr,
     [](lua_State* state, const E& event) {
         entity_lifetime_integer(state, event, event.damageEntityLifetime.token.slot);
     }},
    {"entity_incarnation",
     nullptr,
     [](lua_State* state, const E& event) {
         entity_lifetime_integer(state, event, event.damageEntityLifetime.token.incarnation);
     }},
    {"allocation_sequence",
     nullptr,
     [](lua_State* state, const E& event) {
         entity_lifetime_integer(state, event, event.damageEntityLifetime.allocationSequence);
     }},
    {"allocation_domain",
     nullptr,
     [](lua_State* state, const E& event) {
         if (event.damageFromEntity) {
             push_u64_string(state, event.damageEntityLifetime.allocationDomain);
         } else {
             lua_pushnil(state);
         }
     }},
};
// Sense ordinal 1 backs interaction_open; its meaning is assumed.
constexpr EventBinding kObject[]{
    {"generation", nullptr, &integer_field<&E::objectGeneration>},
    {"present", nullptr, &boolean_field<&E::objectPresent>},
    {"alive", nullptr, &boolean_field<&E::objectPresent>},
    {"interaction_open", nullptr, &boolean_field<&E::objectAlive>},
    {"owner_known", nullptr, &boolean_field<&E::objectOwnerKnown>},
    {"has_owner", nullptr, &boolean_field<&E::objectHasOwner>},
    {"owner_key",
     nullptr,
     [](lua_State* state, const E& event) {
         if (event.objectOwnerKey != 0) {
             push_u64_string(state, event.objectOwnerKey);
         } else {
             lua_pushnil(state);
         }
     }},
    {"initial_observation", nullptr, &boolean_field<&E::initialObservation>},
    {"entry_index", nullptr, &integer_field<&E::objectEntryIndex>},
    {"spawn_mask_low",
     nullptr,
     [](lua_State* state, const E& event) { lua_pushinteger(state, event.objectSpawnMask[0]); }},
    {"spawn_mask_high",
     nullptr,
     [](lua_State* state, const E& event) { lua_pushinteger(state, event.objectSpawnMask[1]); }},
};
constexpr EventBinding kGhostLink[]{
    {"generation", nullptr, &integer_field<&E::ghostGeneration>},
    {"progress", nullptr, &number_field<&E::ghostProgress>},
    {"active", nullptr, &boolean_field<&E::ghostActive>},
};
// A provocation carries the exact squad reference, not a native Sense body.
constexpr EventBinding kSquadProvoked[]{
    {"registry_key", nullptr, &integer_field<&E::firstRegistryKey>},
    {"slot_type", nullptr, &integer_field<&E::firstSlotType>},
    {"slot_index", nullptr, &integer_field<&E::firstSlotIndex>},
};
constexpr EventBinding kFireteam[]{
    {"alive_count", nullptr, &integer_field<&E::fireteamAlive>},
    {"dead_count", nullptr, &integer_field<&E::fireteamDead>},
    {"unknown_count", nullptr, &integer_field<&E::fireteamUnknown>},
};
// Copied from the exact Sense observation, never from an actor token.
constexpr EventBinding kSenseGeneration[]{
    {"spawn_generation",
     nullptr,
     &present_integer<&E::squadSpawnGeneration, &E::squadHasSpawnGeneration>},
    {"sense_generation",
     nullptr,
     &present_integer<&E::senseGenerationPlusOne, &E::hasSenseGeneration>},
};
constexpr EventBinding kSquadRemoval[]{
    {"removal_flag", nullptr, &boolean_field<&E::squadRemovalFlag>},
};
// An invalidation carries no level, so its level fields read nil.
constexpr EventBinding kTriggerLevel[]{
    {"available", nullptr, &boolean_field<&E::triggerAvailable>},
    {"continuity",
     nullptr,
     [](lua_State* state, const E& event) {
         lua_pushstring(state,
                        trigger_observation::name(
                            static_cast<trigger_observation::Continuity>(event.triggerContinuity)));
     }},
    {"occupied", nullptr, &present_boolean<&E::triggerOccupied, &E::triggerAvailable>},
    {"member_count", nullptr, &present_integer<&E::triggerCount, &E::triggerAvailable>},
    {"value", nullptr, &present_integer<&E::triggerValue, &E::triggerAvailable>},
    {"all_inside", nullptr, &present_boolean<&E::triggerAll, &E::triggerAvailable>},
};

/** Every group's bindings, indexed by FieldGroupId. */
constexpr std::array<EventBindings, static_cast<std::size_t>(G::count)> kGroupBindings{{
    kCommon,
    kMissionSequence,
    kStateRevision,
    kSlotIdentity,
    kFirstObject,
    kClientInput,
    kPeerHeard,
    kSenseDecode,
    kClientState,
    kIncidentBody,
    kClientMessage,
    kScriptableRevision,
    kIncidentRevision,
    kEffect,
    kRegion,
    kPlayerTrigger,
    kCinematic,
    kTrigger,
    kSquadAlive,
    kSquadState,
    kEntitySpawned,
    kSceneFinished,
    kSceneOutput,
    kObjectiveProgress,
    kPhase,
    kTimer,
    kEntitySlots,
    kSessionPeer,
    kJoinedRevision,
    kActorPath,
    kDevice,
    kDamage,
    kObject,
    kGhostLink,
    kSquadProvoked,
    kFireteam,
    kSenseGeneration,
    kSquadRemoval,
    kTriggerLevel,
}};
static_assert([]() consteval {
    for (std::size_t index = 0; index < kGroupBindings.size(); ++index) {
        if (!bound_to<host::Event>(api::kFieldGroups[index].members, kGroupBindings[index])) {
            return false;
        }
    }
    return true;
}());
static_assert(std::size(api::kEvents) == host::kEventKindCount);

/** Reads one member of any event view through the field groups its kind declares. */
[[nodiscard]] int event_index(lua_State* state) {
    const host::Event& event = check_event(state, 1);
    const std::string_view key = lua_string_view(state, 2);
    const auto kind = static_cast<std::size_t>(event.kind);
    if (kind < std::size(api::kEvents)) {
        for (const api::FieldGroup* group : api::kEvents[kind].groups) {
            const auto id = static_cast<std::size_t>(group - api::kFieldGroups.data());
            if (serve<host::Event>(state, kGroupBindings[id], key, event)) {
                return 1;
            }
        }
    }
    lua_pushnil(state);
    return 1;
}

} // namespace

/** Installs the event view metatable; every kind shares it. */
void register_event_metatables(lua_State* state) {
    register_metatable(state, kEventMetatable, &event_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
