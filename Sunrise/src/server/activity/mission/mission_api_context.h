#pragma once

#include <cstdint>

#include "../../../middleware/bap/activity_message/incident.h"
#include "../../../state/activity/membership/definition.h"
#include "../../../state/activity/mission/definition.h"
#include "mission_api_types.h"
#include "mission_api_values.h"

// The mission context, the state view, request keys, names, lifetime, peers, cohort and the
// context.sdk makers and enums.

namespace sunrise::server::activity::mission::api {

namespace mission_state = ::sunrise::state::activity::mission;

// A name leaves room for its terminator; a string value likewise.
inline constexpr std::int64_t kMaximumNameBytes =
    static_cast<std::int64_t>(mission_state::kStateKeyByteCapacity) - 1;
inline constexpr std::int64_t kMaximumScalarStringBytes =
    static_cast<std::int64_t>(mission_state::kVariableStringByteCapacity) - 1;
/** One cohort holds at most 64 distinct authored squads. */
inline constexpr std::int64_t kCohortCapacity = 64;

// --- Context -----------------------------------------------------------------------------------

// Argument shapes of the context members, in member order.
inline constexpr ValueSpec kExactObjectSelector[]{{.kind = ValueKind::selector,
                                                   .ref = "mission.Slot",
                                                   .slot = kExactObject,
                                                   .handleAccepted = true}};
inline constexpr ValueSpec kActivateObjectsArguments[]{
    {.name = "slots",
     .kind = ValueKind::list,
     .element = kExactObjectSelector,
     .note = "dense list; a slot may appear once; grouped into one request per object"},
    kActiveArgument,
};
inline constexpr ValueSpec kRequestKeyList[]{
    {.kind = ValueKind::list, .element = kRequestKey, .note = "one key per object group"}};
inline constexpr ValueSpec kSquadSelector[]{
    {.kind = ValueKind::selector, .ref = "mission.Squad", .handleAccepted = true}};
inline constexpr ValueSpec kCohortArguments[]{
    {.name = "squads",
     .kind = ValueKind::list,
     .bound = between(1, kCohortCapacity),
     .element = kSquadSelector,
     .note = "dense list; a squad may appear once"},
};
inline constexpr ValueSpec kCohortHandle[]{{.kind = ValueKind::handle, .ref = "cohort"}};
inline constexpr ValueSpec kSquadLookupArguments[]{
    {.name = "selector", .kind = ValueKind::selector, .ref = "mission.Squad"}};
inline constexpr ValueSpec kSquadHandle[]{{.kind = ValueKind::handle, .ref = "squad"}};
inline constexpr ValueSpec kSceneLookupArguments[]{
    {.name = "selector", .kind = ValueKind::selector, .ref = "mission.Scene"}};
inline constexpr ValueSpec kSceneHandle[]{{.kind = ValueKind::handle, .ref = "scene"}};
inline constexpr ValueSpec kSlotLookupArguments[]{
    {.name = "selector", .kind = ValueKind::selector, .ref = "mission.Slot"}};
inline constexpr ValueSpec kHoldSpawnArguments[]{
    {.name = "active", .kind = ValueKind::boolean, .note = "true holds the spawn, false releases"},
};
inline constexpr ValueSpec kSendIncidentArguments[]{
    {.name = "target",
     .kind = ValueKind::integer,
     .bound = between(0, middleware::bap::activity_message::incident::kTargetMaximum),
     .note = "sobject row the client resolves to an incident handler; poisoned rows are refused. "
             "require(\"sunrise.incidents\").Notification names every notification"},
    {.name = "payload",
     .kind = ValueKind::string,
     .optional = true,
     .bound = at_most(middleware::bap::activity_message::incident::kPayloadMaximum),
     .note = "raw bytes; absent sends the all-zero 42-byte header, \"\" sends none"},
};
inline constexpr ValueSpec kStateRowFields[]{
    {.name = "region_index", .kind = ValueKind::integer, .bound = between(0, kInt32Maximum)},
};
inline constexpr ValueSpec kSelectStateOptionFields[]{
    {.name = "retire_placed_props",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "ends the map props captured from the old state before the move"},
};
inline constexpr ValueSpec kSelectStateArguments[]{
    {.name = "state",
     .kind = ValueKind::missionRow,
     .ref = "mission.states",
     .fields = kStateRowFields},
    {.name = "options",
     .kind = ValueKind::table,
     .optional = true,
     .bound = at_most(static_cast<std::int64_t>(mission_state::kMissionSeedOmitCapacity)),
     .element = kSlotSelector,
     .fields = kSelectStateOptionFields,
     .note = "array part: slots kept out of the new seed; other keys are refused"},
};
inline constexpr ValueSpec kRestartCheckpointArguments[]{
    {.name = "region",
     .kind = ValueKind::integer,
     .bound = between(0, ::sunrise::state::activity::membership::kMaximumSliceSetIndex),
     .note = "the region the party is in"},
    {.name = "spawn_set_hash",
     .kind = ValueKind::integer,
     .bound = between(1, kUint32Maximum - 1),
     .note = "the authored spawn set to restart at"},
    {.name = "release_request",
     .kind = ValueKind::decimalString,
     .optional = true,
     .bound = at_least(1),
     .note = "the value of the arming request key; releases the wipe it armed"},
};
inline constexpr ValueSpec kPhaseArguments[]{
    {.name = "phase", .kind = ValueKind::integer, .bound = uint32_range()}};
inline constexpr ValueSpec kName{
    .name = "name", .kind = ValueKind::name, .bound = between(1, kMaximumNameBytes)};
inline constexpr ValueSpec kNameArguments[]{kName};
inline constexpr ValueSpec kSetVariableArguments[]{
    kName,
    {.name = "value",
     .kind = ValueKind::scalar,
     .bound = at_most(kMaximumScalarStringBytes),
     .note = "nil is refused; use clear_variable"},
};
inline constexpr ValueSpec kStartTimerArguments[]{
    kName,
    {.name = "delay_ms", .kind = ValueKind::integer, .bound = uint32_range()},
};
inline constexpr ValueSpec kTimerSequence[]{
    {.kind = ValueKind::decimalString, .note = "the timer sequence"}};
inline constexpr ValueSpec kActivityRole[]{
    {.kind = ValueKind::string, .note = "\"public\" or \"private\""}};
inline constexpr ValueSpec kSdkHandle[]{{.kind = ValueKind::handle, .ref = "sdk"}};
inline constexpr ValueSpec kLifetimeHandle[]{{.kind = ValueKind::handle, .ref = "lifetime"}};
inline constexpr ValueSpec kPeersHandle[]{{.kind = ValueKind::handle, .ref = "peers"}};
inline constexpr ValueSpec kModConfigHandle[]{{.kind = ValueKind::handle, .ref = "mod_config"}};
inline constexpr ValueSpec kTimersHandle[]{{.kind = ValueKind::handle, .ref = "timers"}};
inline constexpr ValueSpec kVariablesHandle[]{{.kind = ValueKind::handle, .ref = "variables"}};

/** Members of the mission context, in __index order. */
inline constexpr MemberSpec kContextMembers[]{
    field("sdk_build_id", kString, "sha256:<64 hex>"),
    field("activity_id", kString),
    field("activity_row", kInteger),
    field("definition_hash", kInteger),
    field("activity_role", kActivityRole),
    field("player_key", kDecimal, "the linked player's key"),
    field("sdk", kSdkHandle),
    field("lifetime", kLifetimeHandle),
    field("attempt_generation", kDecimal),
    field("mission_complete", kBoolean, "true after complete_mission was accepted"),
    {.name = "complete_mission",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .returns = kRequestKey,
     .effect = "lifetime.set",
     .note = "sets lifetime state 6; errors when the attempt is already complete"},
    {.name = "activate_objects",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kActivateObjectsArguments,
     .returns = kRequestKeyList,
     .effect = "slot.set_object_active"},
    {.name = "cohort",
     .kind = MemberKind::lookup,
     .call = CallForm::named,
     .arguments = kCohortArguments,
     .returns = kCohortHandle},
    field("peers", kPeersHandle),
    {.name = "squad",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kSquadLookupArguments,
     .returns = kSquadHandle},
    {.name = "scene",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kSceneLookupArguments,
     .returns = kSceneHandle},
    {.name = "slot",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kSlotLookupArguments,
     .returns = kAnySlot},
    {.name = "hold_spawn",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kHoldSpawnArguments,
     .returns = kRequestKey,
     .effect = "mission.hold_spawn"},
    {.name = "send_incident",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kSendIncidentArguments,
     .returns = kRequestKey,
     .effect = "mission.send_incident",
     .note = "sends one msg-19 activity incident to the client, as the activity host would"},
    {.name = "select_state",
     .kind = MemberKind::request,
     .call = CallForm::positional,
     .arguments = kSelectStateArguments,
     .returns = kRequestKey,
     .effect = "mission.select_state",
     .note = "loads another authored state and moves the player when needed"},
    {.name = "restart_checkpoint",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kRestartCheckpointArguments,
     .returns = kRequestKey,
     .effect = "mission.restart_checkpoint",
     .note = "arms a hard wipe at a spawn set, or releases one with release_request"},
    {.name = "set_phase",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kPhaseArguments,
     .note = "a change raises phase_entered"},
    {.name = "set_variable",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kSetVariableArguments},
    {.name = "clear_variable",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kBoolean,
     .note = "true when the variable existed"},
    {.name = "start_timer",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kStartTimerArguments,
     .returns = kTimerSequence,
     .note = "replaces a timer with the same name"},
    {.name = "cancel_timer",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kBoolean,
     .note = "true when the timer existed"},
    field("mod_config", kModConfigHandle),
    field("timers", kTimersHandle),
    field("variables", kVariablesHandle),
};

// --- State view, request keys and names ---------------------------------------------------------

// Shapes the state view returns.
inline constexpr ValueSpec kOptionalScalar[]{{.kind = ValueKind::scalar, .optional = true}};
inline constexpr ValueSpec kTimerDeadline[]{
    {.kind = ValueKind::decimalString, .optional = true, .note = "the deadline tick"}};

/** Members of the read-only state view. */
inline constexpr MemberSpec kStateMembers[]{
    field("phase", kInteger, "includes a set_phase made earlier in this callback"),
    field("revision", kDecimal, "the committed state revision"),
    {.name = "variable",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kOptionalScalar,
     .note = "sees changes made earlier in this callback"},
    {.name = "has_variable",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kBoolean},
    {.name = "timer",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kTimerDeadline},
};

// The other key a request key compares against.
inline constexpr ValueSpec kMatchesArguments[]{
    {.name = "other", .kind = ValueKind::handle, .ref = "request_key"}};

/** Members of one request key. */
inline constexpr MemberSpec kRequestKeyMembers[]{
    {.name = "matches",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kMatchesArguments,
     .returns = kBoolean,
     .note = "true when both keys name the same request"},
    field("value", kDecimal, "store this to match a result after a reload"),
};

// Handles the name collections mint.
inline constexpr ValueSpec kTimerRefHandle[]{{.kind = ValueKind::handle, .ref = "timer_ref"}};
inline constexpr ValueSpec kVariableRefHandle[]{{.kind = ValueKind::handle, .ref = "variable_ref"}};

/** Members of context.timers. */
inline constexpr MemberSpec kTimersMembers[]{
    {.name = "resolve",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kTimerRefHandle},
    field("capacity", kInteger, "the timer store size"),
};

/** Members of context.variables. */
inline constexpr MemberSpec kVariablesMembers[]{
    {.name = "resolve",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kNameArguments,
     .returns = kVariableRefHandle},
    field("capacity", kInteger, "the variable store size"),
};

/** Members of one checked timer or variable name. */
inline constexpr MemberSpec kNameRefMembers[]{field("name", kString)};

// --- Lifetime, peers and cohort ----------------------------------------------------------------

// The lifetime state a set request names.
inline constexpr ValueSpec kLifetimeSetArguments[]{
    {.name = "state", .kind = ValueKind::enumValue, .ref = "lifetime_states"}};

/** Members of context.lifetime. */
inline constexpr MemberSpec kLifetimeMembers[]{
    {.name = "set",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kLifetimeSetArguments,
     .returns = kRequestKey,
     .effect = "lifetime.set",
     .note = "prefer complete_mission for state 6"},
};

// One-based peer row and the view it mints.
inline constexpr ValueSpec kPeerAtArguments[]{
    {.name = "index", .kind = ValueKind::integer, .bound = at_least(1), .note = "up to count"}};
inline constexpr ValueSpec kPeerHandle[]{{.kind = ValueKind::handle, .ref = "peer"}};

/** Members of context.peers. */
inline constexpr MemberSpec kPeersMembers[]{
    field("count", kInteger),
    {.name = "at",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kPeerAtArguments,
     .returns = kPeerHandle},
};

/** Members of one peer; a read after its session left errors. */
inline constexpr MemberSpec kPeerMembers[]{
    field("session_id", kDecimal),
    field("session_generation", kDecimal),
    field("member_key", kDecimal),
    field("join_identity", kDecimal),
};

// The type of cohort.alive_count.
inline constexpr ValueSpec kCohortAlive[]{
    {.kind = ValueKind::integer, .optional = true, .note = "nil while any squad is unknown"}};

/** Members of one cohort. A squad with a queued placement counts as unknown. */
inline constexpr MemberSpec kCohortMembers[]{
    field("alive_count", kCohortAlive),
    field("observed_full", kBoolean, "every squad was seen at full strength"),
    field("cleared", kBoolean, "seen full, and now all dead"),
    field("size", kInteger),
};

// --- context.sdk makers and enums ---------------------------------------------------------------

// Handles of the closed collections and the values the makers mint.
inline constexpr ValueSpec kDeviceChannelsHandle[]{
    {.kind = ValueKind::handle, .ref = "device_channels"}};
inline constexpr ValueSpec kDeviceTransitionsHandle[]{
    {.kind = ValueKind::handle, .ref = "device_transitions"}};
inline constexpr ValueSpec kLifetimeStatesHandle[]{
    {.kind = ValueKind::handle, .ref = "lifetime_states"}};
inline constexpr ValueSpec kSquadModesHandle[]{{.kind = ValueKind::handle, .ref = "squad_modes"}};
inline constexpr ValueSpec kBoundedLanesHandle[]{
    {.kind = ValueKind::handle, .ref = "bounded_lanes"}};
inline constexpr ValueSpec kAtomKindsTable[]{
    {.kind = ValueKind::table, .ref = "atom_kinds", .note = "each name maps to itself"}};
inline constexpr ValueSpec kUnitArguments[]{
    {.name = "value", .kind = ValueKind::number, .note = "the client owns its meaning"}};
inline constexpr ValueSpec kPositionArguments[]{
    {.name = "x", .kind = ValueKind::number, .note = "finite"},
    {.name = "y", .kind = ValueKind::number, .note = "finite"},
    {.name = "z", .kind = ValueKind::number, .note = "finite"},
};
inline constexpr ValueSpec kWorldPositionHandle[]{
    {.kind = ValueKind::handle, .ref = "world_position"}};

/** The context.sdk members this table owns; the live SDK views own the rest. */
inline constexpr MemberSpec kSdkMembers[]{
    field("build_id", kString),
    field("activity_id", kString),
    field("activity_row", kInteger),
    field("definition_hash", kInteger),
    field("atom_kinds", kAtomKindsTable),
    field("client_teleport_reset", kInteger, "the teleport_state of a finished spawn"),
    field("device_channels", kDeviceChannelsHandle),
    field("device_transitions", kDeviceTransitionsHandle),
    field("lifetime_states", kLifetimeStatesHandle),
    field("squad_modes", kSquadModesHandle),
    {.name = "unit",
     .kind = MemberKind::maker,
     .call = CallForm::positional,
     .colon = false,
     .arguments = kUnitArguments,
     .returns = kUnitScalar},
    field("bounded_lanes", kBoundedLanesHandle),
    {.name = "position",
     .kind = MemberKind::maker,
     .call = CallForm::positional,
     .colon = false,
     .arguments = kPositionArguments,
     .returns = kWorldPositionHandle},
};

} // namespace sunrise::server::activity::mission::api
