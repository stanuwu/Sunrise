#pragma once

#include <cstdint>

#include "../../../middleware/bap/activity_message/combatant_auth.h"
#include "../../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../../middleware/bap/activity_message/map_generator_auth.h"
#include "../../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../../middleware/bap/activity_message/music_section_auth.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/bap/activity_message/squad_attachment_auth.h"
#include "../../../middleware/bap/activity_message/squad_objective_auth.h"
#include "../../../middleware/bap/activity_message/volume_toggle_auth.h"
#include "../../../state/activity/mission/definition.h"
#include "../../../state/activity_sdk/format.h"
#include "mission_api_types.h"
#include "mission_api_values.h"

// Every member of a slot handle. A method's gate is the slot type and the properties it checks.

namespace sunrise::server::activity::mission::api {

namespace slot_auth = middleware::bap::activity_message::scriptable_auth;
namespace darkness_zone = middleware::bap::activity_message::darkness_zone;

// --- Slot gates --------------------------------------------------------------------------------
// One gate per method family; the VM checks the same properties.
inline constexpr SlotGate kExactDevice = exact_sensed_gate(slot_format::kDeviceSlotType);
inline constexpr SlotGate kExactCombatant = exact_sensed_gate(slot_auth::kType2SlotType);
inline constexpr SlotGate kExactTrigger = exact_gate(slot_auth::kType31SlotType);
inline constexpr SlotGate kExactDirective = exact_gate(slot_format::kDirectiveSlotType);
inline constexpr SlotGate kExactGhostLink =
    exact_sensed_gate(middleware::bap::activity_message::ghost_link::kSlotType);
inline constexpr SlotGate kExactObjective = exact_class_gate(slot_format::kObjectiveSlotType);
// The path gate checks type, class and the exact flag, but no Auth schema.
inline constexpr SlotGate kActorPath{
    .slotType = middleware::bap::activity_message::combatant_auth::kPathSlotType,
    .checks = {.componentClass = true, .exactJoin = true}};
inline constexpr SlotGate kExactDialogue{
    .slotType = slot_format::kDialogueSlotType,
    .checks = {
        .componentClass = true, .authSchema = true, .exactJoin = true, .dialogueCues = true}};

// --- Slot fields --------------------------------------------------------------------------------

// Fields the Auth catalog supplies; nil when the slot's type and schema have no catalog entry.
inline constexpr ValueSpec kAuthBits[]{
    {.kind = ValueKind::integer, .optional = true, .note = "nil without an Auth catalog entry"}};
inline constexpr ValueSpec kAuthOffset[]{
    {.kind = ValueKind::integer,
     .optional = true,
     .note = "nil without a catalog entry or a contiguous mirror"}};
inline constexpr ValueSpec kAuthFlag[]{
    {.kind = ValueKind::boolean, .optional = true, .note = "nil without an Auth catalog entry"}};
inline constexpr ValueSpec kAuthType[]{{.kind = ValueKind::string,
                                        .optional = true,
                                        .note = "the Auth type name, for example device_sensor"}};

// --- Slot method arguments ----------------------------------------------------------------------
inline constexpr ValueSpec kVolume[]{
    {.kind = ValueKind::handle, .ref = "slot", .slot = type_gate(slot_auth::kType60SlotType)}};
inline constexpr ValueSpec kFilterObject[]{
    {.kind = ValueKind::handle, .ref = "slot", .slot = type_gate(slot_format::kObjectSlotType)}};
inline constexpr ValueSpec kObjectFilterArguments[]{
    {.name = "players",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "adds a test: objects that are players"},
    {.name = "target",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_format::kObjectSlotType),
     .note = "adds a test: this one object"},
    {.name = "targets",
     .kind = ValueKind::list,
     .optional = true,
     .bound = between(1, static_cast<std::int64_t>(slot_auth::kType34PredicateCapacity)),
     .element = kFilterObject,
     .note = "adds one test per object, for example every crystal a beam hop-on tethers"},
    {.name = "squad",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_format::kSquadSlotType),
     .note = "adds a test: the squad a shield or tether hop-on hosts on"},
    {.name = "inside",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType60SlotType),
     .note = "adds a test: objects inside this volume"},
    {.name = "inside_any",
     .kind = ValueKind::list,
     .optional = true,
     .bound = between(1, 5),
     .element = kVolume,
     .note = "adds a test: objects inside any of these volumes"},
};
inline constexpr ValueSpec kRequiredActive{
    .name = "active", .kind = ValueKind::boolean, .note = "true turns it on, false off"};
inline constexpr ValueSpec kVolumeActiveArguments[]{
    {.name = "volume",
     .kind = ValueKind::handle,
     .ref = "slot",
     .slot = type_gate(slot_auth::kVolumeSlotType),
     .scope = Scope::sameObject,
     .note = "an authored volume of the toggle's own object"},
    kRequiredActive,
};
inline constexpr ValueSpec kSquadAttachmentArguments[]{
    {.name = "source",
     .kind = ValueKind::handle,
     .ref = "slot",
     .slot = exact_gate(slot_format::kSquadSlotType),
     .scope = Scope::sameObject,
     .note = "the squad of the same object the attachment follows"},
    {.name = "spawn_generation",
     .kind = ValueKind::integer,
     .bound = between(1, kInt32Maximum),
     .note = "the source placement's spawn_generation; any other placement is refused as stale"},
    kRequiredActive,
};
inline constexpr ValueSpec kMissionEffectArguments[]{
    {.name = "filter",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType34SlotType),
     .note = "the type-34 filter selecting the hosts; required while enabled"},
    {.name = "enabled",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(true)},
    {.name = "revision",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(1),
     .bound = between(1, kInt32Maximum),
     .note = "raise it to make the client apply an unchanged effect again"},
};
inline constexpr ValueSpec kWatchDamageArguments[]{
    {.name = "target", .kind = ValueKind::handle, .ref = "slot", .slot = kExactObject},
};
inline constexpr ValueSpec kBurstSlot[]{
    {.kind = ValueKind::selector, .ref = "mission.Slot", .slot = kExactObject}};
inline constexpr ValueSpec kObjectActiveArguments[]{
    kActiveArgument,
    {.name = "with",
     .kind = ValueKind::list,
     .optional = true,
     .bound = at_most(static_cast<std::int64_t>(state::activity::mission::kIntentBurstCapacity)),
     .element = kBurstSlot,
     .note = "more objects sent on the same request"},
};
inline constexpr ValueSpec kSnapArgument{.name = "snap",
                                         .kind = ValueKind::boolean,
                                         .optional = true,
                                         .fallback = default_boolean(false),
                                         .note = "jump to the end value instead of moving"};
inline constexpr ValueSpec kChannelArgument{
    .name = "channel", .kind = ValueKind::enumValue, .ref = "device_channels"};
inline constexpr ValueSpec kSetChannelArguments[]{
    kChannelArgument,
    {.name = "value",
     .kind = ValueKind::handle,
     .ref = "unit_scalar",
     .note = "context.sdk.unit(x)"},
    kSnapArgument,
};
inline constexpr ValueSpec kTransitionArguments[]{
    {.name = "transition", .kind = ValueKind::enumValue, .ref = "device_transitions"},
    kSnapArgument,
};
inline constexpr ValueSpec kChannelArguments[]{kChannelArgument};
inline constexpr ValueSpec kOccupancyArguments[]{
    {.name = "filter",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .note = "absent: the client measures its default player set"},
    {.name = "value", .kind = ValueKind::integer, .bound = int32_range()},
};

// Map generator inputs; an omitted field keeps the authored value.
inline constexpr ValueSpec kAnchorFields[]{
    {.name = "first", .kind = ValueKind::integer, .bound = between(-128, 127)},
    {.name = "second", .kind = ValueKind::integer, .bound = between(-128, 127)},
    {.name = "value", .kind = ValueKind::number},
    {.name = "enabled", .kind = ValueKind::boolean},
};
inline constexpr ValueSpec kAnchor[]{{.kind = ValueKind::table, .fields = kAnchorFields}};
inline constexpr ValueSpec kInt32Element[]{{.kind = ValueKind::integer, .bound = int32_range()}};
inline constexpr std::int64_t kAnchorCount =
    middleware::bap::activity_message::map_generator_auth::kAnchorCount;
inline constexpr std::int64_t kGeneratorIntegers =
    middleware::bap::activity_message::map_generator_auth::kIntegerCount;
inline constexpr std::int64_t kGeneratorReals =
    middleware::bap::activity_message::map_generator_auth::kRealCount;
inline constexpr std::int64_t kGeneratorRecords =
    middleware::bap::activity_message::map_generator_auth::kRecordCount;
inline constexpr ValueSpec kGenerateMapArguments[]{
    {.name = "record",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(1),
     .bound = between(1, kGeneratorRecords),
     .note = "which of the two records to write"},
    {.name = "seed",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_text("authored"),
     .bound = at_most(kUint32Maximum),
     .note = "a negative value keeps the authored seed"},
    {.name = "mode",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_text("authored"),
     .bound = between(-128, 127),
     .note = "raw mode byte; meaning unknown"},
    {.name = "anchors",
     .kind = ValueKind::list,
     .optional = true,
     .fallback = default_text("authored"),
     .bound = between(kAnchorCount, kAnchorCount),
     .element = kAnchor,
     .note = "meaning unknown"},
    {.name = "enabled",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_text("authored")},
    {.name = "values",
     .kind = ValueKind::list,
     .optional = true,
     .bound = at_most(kGeneratorIntegers),
     .element = kInt32Element,
     .note = "raw integer inputs; the first two apply unless -1"},
    {.name = "reals",
     .kind = ValueKind::list,
     .optional = true,
     .bound = at_most(kGeneratorReals),
     .element = kNumber,
     .note = "raw number inputs"},
    {.name = "state_key",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(0),
     .bound = uint32_range()},
};

// Directive declaration fields the VM reads from a generated mission.Directive row.
inline constexpr ValueSpec kDirectiveRowFields[]{
    {.name = "slot_row", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "name_hash", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "element", .kind = ValueKind::integer, .bound = between(0, kInt32Maximum)},
};
inline constexpr ValueSpec kSetDirectiveArguments[]{
    {.name = "directive",
     .kind = ValueKind::missionRow,
     .ref = "mission.Directive",
     .scope = Scope::receiver,
     .fields = kDirectiveRowFields,
     .note = "the goal to show; must belong to this sensor"},
    {.name = "state",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(0),
     .bound = between(0, 2)},
    {.name = "navpoint",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType47SlotType),
     .note = "map marker target"},
    {.name = "audience",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType70SlotType),
     .note = "engagement sensor for the mission banner"},
    {.name = "waypoint",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType60SlotType),
     .note = "volume where the marker hides"},
    {.name = "progress",
     .kind = ValueKind::list,
     .optional = true,
     .bound = between(1, 4),
     .element = kInt32Element,
     .note = "counter values; only on a directive whose row has counter = true"},
};
// Darkness zone, music, interaction, Ghost link and public event arguments.
inline constexpr ValueSpec kDarknessZoneArguments[]{
    {.name = "enabled",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "a countdown needs enabled"},
    {.name = "wipe_seconds",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(darkness_zone::kNoWipe),
     .bound = between(darkness_zone::kNoWipe, darkness_zone::kMaximumWipeSeconds),
     .note = "-1 means no wipe countdown"},
};
inline constexpr std::int64_t kMusicSectionCount =
    middleware::bap::activity_message::music_section::kSectionCount;
inline constexpr ValueSpec kMusicSectionArguments[]{
    {.name = "section", .kind = ValueKind::integer, .bound = between(0, kMusicSectionCount - 1)},
    {.name = "enabled",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(true)},
};
inline constexpr ValueSpec kInteractableArguments[]{
    {.name = "track_owner",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false)},
    kActiveArgument,
    {.name = "used",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false)},
};
inline constexpr ValueSpec kActiveTrueArguments[]{kActiveArgument};
inline constexpr ValueSpec kGhostLevelFields[]{
    {.name = "generation", .kind = ValueKind::integer},
    {.name = "progress", .kind = ValueKind::number},
    {.name = "active", .kind = ValueKind::boolean},
};
inline constexpr ValueSpec kGhostLevel[]{{.kind = ValueKind::table,
                                          .optional = true,
                                          .fields = kGhostLevelFields,
                                          .note = "nil before the client reported all three"}};
inline constexpr ValueSpec kPublicEventArguments[]{
    {.name = "state",
     .kind = ValueKind::integer,
     .optional = true,
     .fallback = default_integer(0),
     .bound = int32_range(),
     .note = "shown by the HUD"},
    {.name = "player",
     .kind = ValueKind::decimalString,
     .optional = true,
     .fallback = default_text("context.player_key"),
     .note = "the watched player"},
    {.name = "area",
     .kind = ValueKind::handle,
     .ref = "slot",
     .note = "the object whose zone bounds the event area"},
    {.name = "leave_seconds",
     .kind = ValueKind::number,
     .bound = at_least(0),
     .note = "finite; seconds outside the area before the client reports it"},
};
inline constexpr ValueSpec kTaskGroupRowFields[]{
    {.name = "slot_row", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "group_index",
     .kind = ValueKind::integer,
     .bound = between(0, middleware::bap::activity_message::squad_objective::kTaskGroupCount - 1)},
};
inline constexpr ValueSpec kCombatObjectiveArguments[]{
    {.name = "objective",
     .kind = ValueKind::handle,
     .ref = "slot",
     .slot = kExactObjective,
     .scope = Scope::sameRegistry},
    {.name = "task_group",
     .kind = ValueKind::missionRow,
     .ref = "mission.TaskGroup",
     .optional = true,
     .fallback = default_text("none"),
     .scope = Scope::argument,
     .fields = kTaskGroupRowFields,
     .note = "must belong to objective"},
    {.name = "reconsider",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false),
     .note = "force the squad to pick again"},
    {.name = "reserved",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_text("keeps the current reservation")},
    {.name = "refresh_player_awareness",
     .kind = ValueKind::boolean,
     .optional = true,
     .fallback = default_boolean(false)},
};
inline constexpr ValueSpec kSpawnArgument{
    .name = "spawn",
    .kind = ValueKind::boolean,
    .optional = true,
    .fallback = default_boolean(false),
    .note = "creates the actor from its single authored source first"};
inline constexpr ValueSpec kActorPathArguments[]{
    {.name = "path",
     .kind = ValueKind::handle,
     .ref = "slot",
     .slot = kActorPath,
     .scope = Scope::sameObject,
     .note = "needs an authored destination"},
    kSpawnArgument,
};
inline constexpr ValueSpec kActorActionArguments[]{
    kAbilityArgument,
    kAbilityTargetArgument,
    kSpawnArgument,
};
inline constexpr ValueSpec kAtom[]{{.kind = ValueKind::atom, .ref = "atom_kinds"}};
inline constexpr ValueSpec kRunAtomsArguments[]{
    kSpawnArgument,
    {.name = "atoms",
     .kind = ValueKind::list,
     .bound = between(1, 32),
     .element = kAtom,
     .note = "one lane per atom, run in order"},
};
inline constexpr ValueSpec kActorSequenceArguments[]{
    {.name = "sequence",
     .kind = ValueKind::handle,
     .ref = "actor_sequence",
     .scope = Scope::receiver,
     .note = "a playable sequence of this actor"},
};
inline constexpr ValueSpec kActorSequencesHandle[]{
    {.kind = ValueKind::handle, .ref = "actor_sequences"}};
inline constexpr ValueSpec kDialogueCueArguments[]{
    {.name = "cue",
     .kind = ValueKind::integer,
     .ref = "mission.DialogueCue",
     .bound = between(0, kUint16Maximum),
     .note = "a cue past this slot's cue count is refused on delivery"},
    {.name = "filter",
     .kind = ValueKind::handle,
     .ref = "slot",
     .optional = true,
     .slot = type_gate(slot_auth::kType60SlotType)},
};
inline constexpr ValueSpec kPerformanceStateFields[]{
    {.name = "slot_row", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "name_hash", .kind = ValueKind::integer, .bound = uint32_range()},
};
inline constexpr ValueSpec kPerformanceArguments[]{
    {.name = "state",
     .kind = ValueKind::missionRow,
     .ref = "mission.PerformanceState",
     .optional = true,
     .fallback = default_text("the target's only state"),
     .scope = Scope::receiver,
     .fields = kPerformanceStateFields,
     .note = "may be left out when the target declares exactly one state"},
};

/** Members of one slot handle, in __index order. Every read re-checks the row first. */
inline constexpr MemberSpec kSlotMembers[]{
    field("row", kInteger, "slot row in this activity"),
    {.name = "applied",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .gate = type_gate(slot_format::kDeviceSlotType),
     .arguments = kChannelArguments,
     .returns = kBoolean,
     .note = "true once a report satisfied the latest request on the channel"},
    field("id", kString, "same as the mission.Slot value"),
    field("name", kString),
    field("object_id", kString),
    field("object_tag", kInteger),
    field("registry_key", kInteger),
    field("slot_index", kInteger),
    field("slot_type", kInteger),
    field("component_class", kInteger),
    field("sense_schema", kInteger),
    field("auth_schema", kInteger),
    field("sense_schema_id", kString),
    field("auth_schema_id", kString),
    field("auth_type", kAuthType),
    field("auth_min_bits", kAuthBits),
    field("auth_max_bits", kAuthBits),
    field("auth_component_offset", kAuthOffset),
    field("auth_dynamic", kAuthFlag),
    field("auth_writable", kAuthFlag),
    field("flags", kInteger, "catalog flags"),
    {.name = "set_object_filter",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = schema_gate(slot_auth::kType34SlotType),
     .arguments = kObjectFilterArguments,
     .returns = kRequestKey,
     .note = "all tests share eight predicates; a filter that needs more is refused"},
    {.name = "set_volume_active",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_auth::kType32SlotType),
     .arguments = kVolumeActiveArguments,
     .returns = kRequestKey,
     .note = "turns one authored volume on or off through its type-32 toggle"},
    {.name = "set_squad_attachment",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_gate(slot_auth::kType26SlotType),
     .arguments = kSquadAttachmentArguments,
     .returns = kRequestKey,
     .effect = "slot.set_squad_attachment",
     .note = "selects or clears the squad a type-26 attachment follows"},
    {.name = "set_mission_effect",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = schema_gate(slot_auth::kType26SlotType),
     .arguments = kMissionEffectArguments,
     .returns = kRequestKey,
     .note = "attaches the authored hop-on effect to the entities a type-34 filter selects"},
    {.name = "watch_damage",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = schema_gate(middleware::bap::activity_message::damage_monitor::kSlotType),
     .arguments = kWatchDamageArguments,
     .returns = kRequestKey,
     .effect = "slot.watch_damage"},
    {.name = "set_object_active",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactObject,
     .arguments = kObjectActiveArguments,
     .returns = kRequestKey,
     .effect = "slot.set_object_active"},
    {.name = "set_channel",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactDevice,
     .arguments = kSetChannelArguments,
     .returns = kRequestKey,
     .effect = "slot.set_channel"},
    {.name = "transition",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactDevice,
     .arguments = kTransitionArguments,
     .returns = kRequestKey,
     .effect = "slot.set_channel"},
    {.name = "set_occupancy_condition",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_sensed_gate(slot_format::kOccupancySlotType),
     .arguments = kOccupancyArguments,
     .returns = kRequestKey},
    {.name = "generate_map",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(middleware::bap::activity_message::map_generator_auth::kSlotType),
     .arguments = kGenerateMapArguments,
     .returns = kRequestKey,
     .note = "writes one of the two generator records and leaves the other authored"},
    {.name = "set_directive",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactDirective,
     .arguments = kSetDirectiveArguments,
     .returns = kRequestKey},
    {.name = "clear_directives",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactDirective,
     .returns = kRequestKey,
     .note = "hides the active goal"},
    {.name = "set_darkness_zone",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(darkness_zone::kSlotType),
     .arguments = kDarknessZoneArguments,
     .returns = kRequestKey},
    {.name = "set_music_section",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(middleware::bap::activity_message::music_section::kSlotType),
     .arguments = kMusicSectionArguments,
     .returns = kRequestKey},
    {.name = "set_interactable_object",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactObject,
     .arguments = kInteractableArguments,
     .returns = kRequestKey,
     .effect = "slot.set_interactable_object"},
    {.name = "set_ghost_link",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactGhostLink,
     .arguments = kActiveTrueArguments,
     .returns = kRequestKey,
     .effect = "slot.set_ghost_link"},
    {.name = "ghost_link",
     .kind = MemberKind::query,
     .call = CallForm::named,
     .gate = kExactGhostLink,
     .returns = kGhostLevel},
    {.name = "set_public_event_state",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_gate(slot_auth::kType71SlotType),
     .arguments = kPublicEventArguments,
     .returns = kRequestKey},
    {.name = "assign_combat_objective",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_format::kSquadSlotType),
     .arguments = kCombatObjectiveArguments,
     .returns = kRequestKey,
     .effect = "slot.assign_combat_objective",
     .note = "inside on_event_squad_state for this squad it carries the reported revision"},
    {.name = "play_actor_path",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .arguments = kActorPathArguments,
     .returns = kRequestKey,
     .effect = "slot.run_atoms"},
    {.name = "play_actor_action",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .arguments = kActorActionArguments,
     .returns = kRequestKey,
     .effect = "slot.run_atoms",
     .note = "a one-atom ability program"},
    {.name = "retire_actor",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .returns = kRequestKey,
     .effect = "slot.retire_actor"},
    {.name = "bind_combatant_to_squad",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .returns = kRequestKey,
     .note = "arms the actor for its scene's squad spawn"},
    {.name = "run_atoms",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .arguments = kRunAtomsArguments,
     .returns = kRequestKey,
     .effect = "slot.run_atoms"},
    {.name = "fire_trigger",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactTrigger,
     .returns = kRequestKey,
     .effect = "slot.fire_trigger",
     .note = "arms the trigger"},
    {.name = "disarm_trigger",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactTrigger,
     .returns = kRequestKey,
     .effect = "slot.fire_trigger",
     .note = "stops it reporting"},
    {.name = "play_sequence",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactCombatant,
     .arguments = kActorSequenceArguments,
     .returns = kRequestKey,
     .effect = "slot.play_sequence",
     .note = "the actor form; chosen when the slot is an exact type-2 combatant"},
    {.name = "play_sequence",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_format::kSequenceSlotType),
     .returns = kRequestKey,
     .effect = "slot.play_sequence",
     .note = "plays the authored sequence; chosen for every other slot"},
    {.name = "sequences",
     .kind = MemberKind::lookup,
     .call = CallForm::positional,
     .gate = kExactCombatant,
     .returns = kActorSequencesHandle},
    {.name = "set_cinematic_active",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_format::kCinematicSlotType),
     .arguments = kActiveTrueArguments,
     .returns = kRequestKey,
     .effect = "slot.set_cinematic_active"},
    {.name = "reset_objectives",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_gate(slot_format::kObjectiveSlotType),
     .returns = kRequestKey,
     .effect = "slot.reset_objectives"},
    {.name = "advance_task",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_format::kTaskSlotType),
     .returns = kRequestKey,
     .effect = "slot.advance_task"},
    {.name = "play_dialogue_cue",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = kExactDialogue,
     .arguments = kDialogueCueArguments,
     .returns = kRequestKey,
     .effect = "slot.play_dialogue_cue"},
    {.name = "play_performance",
     .kind = MemberKind::request,
     .call = CallForm::named,
     .gate = exact_class_gate(slot_format::kPerformanceSlotType),
     .arguments = kPerformanceArguments,
     .returns = kRequestKey,
     .effect = "slot.play_performance"},
};

/** @return One slot member; an overloaded name also needs its slot type. */
[[nodiscard]] consteval const MemberSpec& slot_member(std::string_view name,
                                                      std::uint32_t slotType = kAnySlotType) {
    return find_member(kSlotMembers, name, slotType);
}

} // namespace sunrise::server::activity::mission::api
