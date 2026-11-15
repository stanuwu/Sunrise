#pragma once

#include <cstdint>
#include <iterator>

#include "../../../middleware/bap/activity_message/scene_events_auth.h"
#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../state/activity/mission/definition.h"
#include "mission_api_types.h"
#include "mission_api_values.h"

// Squad, scene and the small value handles: count vectors, enum entries, units, positions,
// bounded lanes and actor sequences.

namespace sunrise::server::activity::mission::api {

// --- Squad -------------------------------------------------------------------------------------

// Argument and return shapes of the squad members.
inline constexpr ValueSpec kCountVectorHandle[]{{.kind = ValueKind::handle, .ref = "count_vector"}};
inline constexpr ValueSpec kSquadAnchors[]{
    {.kind = ValueKind::handle,
     .ref = "squad_anchors",
     .optional = true,
     .note = "spawn points as world rows; a live SDK view; nil when the world is stale"}};
inline constexpr ValueSpec kPlaceArguments[]{
    {.name = "counts",
     .kind = ValueKind::handle,
     .ref = "count_vector",
     .optional = true,
     .fallback = default_text("the authored counts"),
     .scope = Scope::receiver,
     .note = "a vector from counts() of this squad"},
    {.name = "mode",
     .kind = ValueKind::enumValue,
     .ref = "squad_modes",
     .optional = true,
     .fallback = default_text("reinforce")},
    {.name = "retire_on_return",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "the host retires the old entities when the client returns them"},
    {.name = "spawn_rule",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(middleware::content::packages::tables::kAuthoredSquadRuleSlotType),
     .note = "replaces the authored spawn rule"},
    {.name = "temperaments",
     .kind = ValueKind::list,
     .optional = true,
     .fallback = default_text("the squad's last list"),
     .bound = kTemperamentBound,
     .element = kTemperament,
     .note = "distinct; every member merges them; empty clears them"},
};
inline constexpr ValueSpec kActorCommandArguments[]{
    {.name = "command",
     .kind = ValueKind::integer,
     .ref = "mission.ActorCommand",
     .bound = uint32_range()},
    {.name = "value",
     .kind = ValueKind::integer,
     .bound = int32_range(),
     .note = "the command value, for example a mission.Faction value"},
};

/** Members of one squad handle, in __index order. Every read re-checks the squad first. */
inline constexpr MemberSpec kSquadMembers[]{
    field("row", kInteger),
    field("id", kString),
    field("name", kString),
    field("member_count", kInteger),
    field("default_counts", kIntegerList, "authored count per member"),
    {.name = "counts",
     .kind = MemberKind::maker,
     .call = CallForm::positional,
     .returns = kCountVectorHandle,
     .note = "a new vector filled with the defaults; errors past eight members"},
    field("anchors", kSquadAnchors),
    {.name = "place",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kPlaceArguments,
     .returns = kRequestKey,
     .effect = "squad.place"},
    {.name = "actor_command",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kActorCommandArguments,
     .returns = kRequestKey,
     .note = "one command to every live member"},
};

// One-based member index and the count written there.
inline constexpr ValueSpec kMemberIndex{
    .name = "member", .kind = ValueKind::integer, .bound = at_least(1), .note = "up to count"};
inline constexpr ValueSpec kCountAtArguments[]{kMemberIndex};
inline constexpr ValueSpec kCountSetArguments[]{
    kMemberIndex,
    {.name = "count", .kind = ValueKind::integer, .bound = between(0, kInt32Maximum)},
};

/** Members of one squad count vector. A vector is bound to the squad that minted it. */
inline constexpr MemberSpec kCountVectorMembers[]{
    field("count", kInteger, "number of members"),
    field("capacity", kInteger, "the client array size"),
    {.name = "at",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kCountAtArguments,
     .returns = kInteger},
    {.name = "set",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kCountSetArguments},
};

// --- Scene -------------------------------------------------------------------------------------

// Argument shapes of the scene members.
inline constexpr ValueSpec kActivateArguments[]{
    {.name = "spawn",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "also spawns the scene's single authored cast"},
    {.name = "initial_event",
     .kind = ValueKind::integer,
     .ref = "mission.SceneInput",
     .optional = true,
     .fallback = default_text("none"),
     .bound = between(1,
                      static_cast<std::int64_t>(
                          middleware::bap::activity_message::scene_events::kInvalidEventKey)
                          - 1),
     .scope = Scope::receiver,
     .note = "an input the scene's activation already carries; a key no block waits on is "
             "refused on delivery"},
};
inline constexpr ValueSpec kSendEventArguments[]{
    {.name = "key",
     .kind = ValueKind::integer,
     .ref = "mission.SceneInput",
     .bound = between(1,
                      static_cast<std::int64_t>(
                          middleware::bap::activity_message::scene_events::kInvalidEventKey)
                          - 1),
     .scope = Scope::receiver,
     .note = "a key no block of this scene waits on is refused on delivery"},
};

/** Members of one authored scene handle. Every read re-checks the scene first. */
inline constexpr MemberSpec kSceneMembers[]{
    field("row", kInteger),
    field("id", kString),
    {.name = "activate",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kActivateArguments,
     .returns = kRequestKey,
     .effect = "scene.activate"},
    {.name = "stop",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .returns = kRequestKey,
     .effect = "scene.stop"},
    {.name = "send_event",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .arguments = kSendEventArguments,
     .returns = kRequestKey,
     .effect = "scene.send_event",
     .note = "fires one input of the running scene"},
};

// --- Enum collections and entries ---------------------------------------------------------------

// Handles a collection mints and the indexes its at() takes.
inline constexpr ValueSpec kDeviceChannelHandle[]{
    {.kind = ValueKind::handle, .ref = "device_channel"}};
inline constexpr ValueSpec kDeviceTransitionHandle[]{
    {.kind = ValueKind::handle, .ref = "device_transition"}};
inline constexpr ValueSpec kTransitionAtArguments[]{
    {.name = "index",
     .kind = ValueKind::integer,
     .bound = between(1, static_cast<std::int64_t>(std::size(kDeviceTransitionEntries)))}};
inline constexpr ValueSpec kLifetimeStateHandle[]{
    {.kind = ValueKind::handle, .ref = "lifetime_state"}};
inline constexpr ValueSpec kLifetimeAtArguments[]{
    {.name = "state", .kind = ValueKind::integer, .bound = between(0, kMaximumLifetimeState)}};

/** Members of context.sdk.device_channels beside the channel names. */
inline constexpr MemberSpec kDeviceChannelsMembers[]{field("count", kInteger)};

/** Members of one device channel. */
inline constexpr MemberSpec kDeviceChannelMembers[]{
    field("name", kString),
    field("value", kInteger, "the type-23 wire lane"),
};

/** Members of context.sdk.device_transitions beside the transition names. */
inline constexpr MemberSpec kDeviceTransitionsMembers[]{
    field("count", kInteger),
    {.name = "at",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kTransitionAtArguments,
     .returns = kDeviceTransitionHandle},
};

/** Members of one device transition. */
inline constexpr MemberSpec kDeviceTransitionMembers[]{
    field("name", kString),
    field("channel", kDeviceChannelHandle),
    field("value", kUnitScalar, "the channel endpoint"),
};

/** Members of context.sdk.squad_modes beside the mode names. */
inline constexpr MemberSpec kSquadModesMembers[]{field("count", kInteger)};

/** Members of one squad mode. */
inline constexpr MemberSpec kSquadModeMembers[]{
    field("name", kString),
    field("value", kInteger, "the squad Auth placement mode"),
};

/** Members of one unit scalar. The maker does not check the range. */
inline constexpr MemberSpec kUnitScalarMembers[]{
    field("value", kNumber),
    field("low", kNumber, "0, the low device endpoint"),
    field("high", kNumber, "1, the high device endpoint"),
};

/** Members of one world position. */
inline constexpr MemberSpec kWorldPositionMembers[]{
    field("x", kNumber),
    field("y", kNumber),
    field("z", kNumber),
};

/** Members of context.sdk.lifetime_states. */
inline constexpr MemberSpec kLifetimeStatesMembers[]{
    field("count", kInteger),
    {.name = "at",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kLifetimeAtArguments,
     .returns = kLifetimeStateHandle},
    field("default", kLifetimeStateHandle, "state 3"),
};

/** Members of one lifetime state. */
inline constexpr MemberSpec kLifetimeStateMembers[]{field("value", kInteger)};

// --- Bounded lanes -----------------------------------------------------------------------------

// The list a lane mints and the shapes its lists take and return.
inline constexpr ValueSpec kLaneList[]{{.kind = ValueKind::handle,
                                        .ref = "count_list",
                                        .note = "an object_ref_list for an object-reference lane"}};
inline constexpr ValueSpec kLaneKind[]{{.kind = ValueKind::string, .note = "count or object_ref"}};
inline constexpr ValueSpec kListRowArguments[]{
    {.name = "row", .kind = ValueKind::integer, .bound = at_least(1), .note = "up to count"}};
inline constexpr ValueSpec kCountAppendArguments[]{
    {.name = "value", .kind = ValueKind::integer, .bound = int32_range()}};
inline constexpr ValueSpec kSlotAppendArguments[]{
    {.name = "slot", .kind = ValueKind::handle, .ref = "slot"}};

/** Members of context.sdk.bounded_lanes beside the lane names. */
inline constexpr MemberSpec kBoundedLanesMembers[]{field("count", kInteger)};

/** Members of one bounded lane. */
inline constexpr MemberSpec kBoundedLaneMembers[]{
    field("name", kString),
    field("capacity", kInteger, "the client array size"),
    field("kind", kLaneKind),
    {.name = "list",
     .kind = MemberKind::maker,
     .call = CallForm::positional,
     .returns = kLaneList,
     .note = "an empty list of this lane"},
};

/** Members of one count list. Its length cannot pass the lane's capacity. */
inline constexpr MemberSpec kCountListMembers[]{
    field("count", kInteger),
    field("capacity", kInteger),
    field("lane", kString),
    {.name = "at",
     .kind = MemberKind::query,
     .call = CallForm::positional,
     .arguments = kListRowArguments,
     .returns = kInteger},
    {.name = "append",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kCountAppendArguments,
     .note = "errors when the list is full"},
};

/** Members of one object reference list. Its length cannot pass the lane's capacity. */
inline constexpr MemberSpec kObjectRefListMembers[]{
    field("count", kInteger),
    field("capacity", kInteger),
    field("lane", kString),
    {.name = "at",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kListRowArguments,
     .returns = kAnySlot},
    {.name = "append",
     .kind = MemberKind::update,
     .call = CallForm::positional,
     .arguments = kSlotAppendArguments,
     .note = "errors when the list is full"},
};

// --- Actor sequences ---------------------------------------------------------------------------

// The ordinal at() takes and the sequence it mints.
inline constexpr ValueSpec kSequenceAtArguments[]{
    {.name = "ordinal", .kind = ValueKind::integer, .bound = at_least(1)}};
inline constexpr ValueSpec kOptionalActorSequence[]{
    {.kind = ValueKind::handle, .ref = "actor_sequence", .optional = true}};
inline constexpr ValueSpec kOptionalText[]{
    {.kind = ValueKind::string, .optional = true, .note = "nil when the extracted text is empty"}};

/** Members of one actor's sequence collection beside the sequence symbols. */
inline constexpr MemberSpec kActorSequencesMembers[]{
    field("count", kInteger),
    {.name = "at",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .arguments = kSequenceAtArguments,
     .returns = kOptionalActorSequence},
};

// --- Activity message rows ---------------------------------------------------------------------

// The event a catalog message row is tested against.
inline constexpr ValueSpec kMessageMatchesArguments[]{
    {.name = "event", .kind = ValueKind::handle, .ref = "event", .note = "any event view"}};

/** The activity message member this table owns; the live SDK views own the row fields. */
inline constexpr MemberSpec kActivityMessageMembers[]{
    {.name = "matches",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .arguments = kMessageMatchesArguments,
     .returns = kBoolean,
     .note = "true when the event is this message; only on a row with a receive route"},
};

/** Members of one actor sequence. */
inline constexpr MemberSpec kActorSequenceMembers[]{
    field("id", kOptionalText),
    field("name", kOptionalText),
    field("symbol", kOptionalText),
    field("source_path", kOptionalText),
    field("key", kInteger, "the key hash"),
    field("kind", kInteger),
    field("resource_tag", kInteger),
    field("table_index", kInteger),
    field("ordinal", kInteger),
    field("source_offset", kInteger),
    field("playable", kBoolean, "only a playable sequence can be played"),
};

} // namespace sunrise::server::activity::mission::api
