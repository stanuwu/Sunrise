#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../../state/activity_sdk/format.h"
#include "mission_api_types.h"

// Shared value shapes and the closed vocabularies under context.sdk.

namespace sunrise::server::activity::mission::api {

namespace slot_format = ::sunrise::state::activity_sdk::format;

/** The gate every exact object-slot argument takes. */
inline constexpr SlotGate kExactObject = exact_sensed_gate(slot_format::kObjectSlotType);

// --- Shared value shapes ------------------------------------------------------------------------
// One-element arrays, so a field or a return names its type by reference.
inline constexpr ValueSpec kBoolean[]{{.kind = ValueKind::boolean}};
inline constexpr ValueSpec kInteger[]{{.kind = ValueKind::integer}};
inline constexpr ValueSpec kNumber[]{{.kind = ValueKind::number}};
inline constexpr ValueSpec kString[]{{.kind = ValueKind::string}};
inline constexpr ValueSpec kDecimal[]{{.kind = ValueKind::decimalString}};
inline constexpr ValueSpec kOptionalInteger[]{{.kind = ValueKind::integer, .optional = true}};
inline constexpr ValueSpec kOptionalNumber[]{{.kind = ValueKind::number, .optional = true}};
inline constexpr ValueSpec kOptionalString[]{{.kind = ValueKind::string, .optional = true}};
inline constexpr ValueSpec kOptionalDecimal[]{{.kind = ValueKind::decimalString, .optional = true}};
inline constexpr ValueSpec kIntegerList[]{
    {.kind = ValueKind::list, .element = kInteger, .note = "one entry per member, from 1"}};
inline constexpr ValueSpec kRequestKey[]{{.kind = ValueKind::handle, .ref = "request_key"}};
inline constexpr ValueSpec kOptionalRequestKey[]{
    {.kind = ValueKind::handle, .ref = "request_key", .optional = true}};
inline constexpr ValueSpec kAnySlot[]{{.kind = ValueKind::handle, .ref = "slot"}};
inline constexpr ValueSpec kSlotSelector[]{{.kind = ValueKind::selector, .ref = "mission.Slot"}};
inline constexpr ValueSpec kUnitScalar[]{{.kind = ValueKind::handle, .ref = "unit_scalar"}};

// The optional active argument several requests share.
inline constexpr ValueSpec kActiveArgument{.name = "active",
                                           .kind = ValueKind::boolean,
                                           .optional = true,
                                           .fallback = default_boolean(true)};

// --- Device channels and transitions -----------------------------------------------------------

using Type23Channel = middleware::bap::activity_message::scriptable_auth::Type23Channel;

// The three type-23 lanes, by their wire value.
inline constexpr EnumEntry kDeviceChannelEntries[]{
    {.name = "position", .value = static_cast<std::int64_t>(Type23Channel::devicePosition)},
    {.name = "power", .value = static_cast<std::int64_t>(Type23Channel::devicePower)},
    {.name = "lock", .value = static_cast<std::int64_t>(Type23Channel::deviceLock)},
};

// Each transition names one lane and one of its two endpoints: 0 is low, 1 is high.
inline constexpr EnumEntry kDeviceTransitionEntries[]{
    {.name = "open", .value = 1, .detail = "position"},
    {.name = "close", .value = 0, .detail = "position"},
    {.name = "power_on", .value = 1, .detail = "power"},
    {.name = "power_off", .value = 0, .detail = "power"},
    {.name = "lock", .value = 1, .detail = "lock"},
    {.name = "unlock", .value = 0, .detail = "lock"},
};

/** @return The wire value of one device channel name. Fails compilation for an unknown name. */
[[nodiscard]] consteval std::uint8_t device_channel_value(std::string_view name) {
    for (const EnumEntry& entry : kDeviceChannelEntries) {
        if (entry.name == name) {
            return static_cast<std::uint8_t>(entry.value);
        }
    }
    throw "device channel is not declared";
}

// --- Squad modes and lifetime states -----------------------------------------------------------

using SquadMode = middleware::bap::activity_message::squad_auth::Mode;

// The squad Auth placement modes; script names for the wire values.
inline constexpr EnumEntry kSquadModeEntries[]{
    {.name = "reinforce", .value = static_cast<std::int64_t>(SquadMode::mode0)},
    {.name = "replace", .value = static_cast<std::int64_t>(SquadMode::mode2)},
    {.name = "reserve", .value = static_cast<std::int64_t>(SquadMode::reserve)},
};

// Lifetime states past 10 index beyond the client jump table; 3 is the client default.
inline constexpr std::int64_t kMaximumLifetimeState = 10;
inline constexpr std::int64_t kDefaultLifetimeState = 3;

// --- Bounded lanes -----------------------------------------------------------------------------

/** What a lane's elements are: plain counted values, or references to installed objects. */
enum class BoundedLaneKind : std::uint8_t {
    count,
    objectRef,
};

/** One fixed client array a body's count or index lane reaches. */
struct BoundedLane final {
    std::string_view name{};
    BoundedLaneKind kind{};
    std::uint16_t capacity{};
};

/**
 * Every lane whose array size is proved. The bound is the array the client indexes, never the
 * width of the wire lane; a lane with no proved array size is absent from this table.
 */
inline constexpr std::array<BoundedLane, 7> kBoundedLanes{{
    {"gameplay_switches", BoundedLaneKind::count, 50},
    {"nav_nodes", BoundedLaneKind::count, 96},
    {"nav_node_links", BoundedLaneKind::count, 16},
    {"type8_field_1_0", BoundedLaneKind::count, 32},
    {"type8_field_2_0", BoundedLaneKind::count, 16},
    {"type8_object_refs", BoundedLaneKind::objectRef, 16},
    {"type53_object_refs", BoundedLaneKind::objectRef, 129},
}};

/** @return The schema word for one lane kind. */
[[nodiscard]] constexpr std::string_view bounded_lane_kind_name(BoundedLaneKind kind) noexcept {
    return kind == BoundedLaneKind::count ? "count" : "object_ref";
}

// --- Atom kinds --------------------------------------------------------------------------------

// Fields every atom table may carry beside its kind's own fields.
inline constexpr ValueSpec kAtomKindField{
    .name = "kind", .kind = ValueKind::string, .ref = "atom_kinds", .note = "an atom_kinds name"};
inline constexpr ValueSpec kAtomQuantizedField{.name = "quantized",
                                               .kind = ValueKind::integer,
                                               .optional = true,
                                               .bound = between(0, 2047),
                                               .note = "11-bit secondary lane value"};

// Fields of the point and move atom kinds.
inline constexpr ValueSpec kAtomTargetField{.name = "target",
                                            .kind = ValueKind::handle,
                                            .ref = "slot",
                                            .note = "slot type 0 to 126, slot index 0 to 32767"};
inline constexpr ValueSpec kAtomPointField{.name = "value",
                                           .kind = ValueKind::integer,
                                           .optional = true,
                                           .fallback = default_integer(0),
                                           .bound = between(0, 255),
                                           .scope = Scope::argument,
                                           .note = "an authored point parameter of target"};
inline constexpr ValueSpec kAtomEnabledField{.name = "enabled",
                                             .kind = ValueKind::boolean,
                                             .optional = true,
                                             .fallback = default_boolean(false),
                                             .note = "any truthy value counts as true"};

// The field set of each atom kind, kind and quantized included.
inline constexpr ValueSpec kAtomPointFields[]{
    kAtomKindField, kAtomTargetField, kAtomPointField, kAtomQuantizedField};
inline constexpr ValueSpec kAtomMoveFields[]{
    kAtomKindField, kAtomTargetField, kAtomPointField, kAtomEnabledField, kAtomQuantizedField};
inline constexpr ValueSpec kAtomSequenceFields[]{
    kAtomKindField,
    {.name = "value", .kind = ValueKind::integer, .bound = uint32_range()},
    kAtomQuantizedField};
inline constexpr ValueSpec kAtomSleepFields[]{
    kAtomKindField,
    {.name = "seconds", .kind = ValueKind::number, .note = "finite"},
    kAtomQuantizedField};
inline constexpr ValueSpec kAtomTrivialFields[]{kAtomKindField, kAtomQuantizedField};
inline constexpr ValueSpec kAtomControlFlagFields[]{kAtomKindField,
                                                    {.name = "value",
                                                     .kind = ValueKind::integer,
                                                     .optional = true,
                                                     .fallback = default_integer(0),
                                                     .bound = between(0, 63)},
                                                    kAtomQuantizedField};
inline constexpr ValueSpec kAtomTemperamentFields[]{
    kAtomKindField,
    {.name = "value", .kind = ValueKind::integer, .bound = uint32_range()},
    kAtomEnabledField,
    kAtomQuantizedField};
inline constexpr ValueSpec kAtomChannelFields[]{
    kAtomKindField,
    {.name = "channel", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "value", .kind = ValueKind::number, .note = "finite"},
    kAtomQuantizedField};
inline constexpr ValueSpec kActorAbilityRowFields[]{
    {.name = "slot_row", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "group_hash", .kind = ValueKind::integer, .bound = uint32_range()},
    {.name = "request_hash", .kind = ValueKind::integer, .bound = uint32_range()},
};
// The ability and target of an ability atom and of play_actor_action.
inline constexpr ValueSpec kAbilityArgument{.name = "ability",
                                            .kind = ValueKind::missionRow,
                                            .ref = "mission.ActorAbility",
                                            .scope = Scope::receiver,
                                            .fields = kActorAbilityRowFields,
                                            .note = "an ability of this actor"};
inline constexpr ValueSpec kAbilityTargetArgument{.name = "target",
                                                  .kind = ValueKind::handle,
                                                  .ref = "slot",
                                                  .optional = true,
                                                  .note =
                                                      "a type-58 point or type-48 point set the "
                                                      "SDK proved; an ability atom's value picks "
                                                      "its point"};
// Which authored point of `target` to aim at. It is only sent with a target, so it is never
// negative here; an atom without a target sends -1 in the lane's signed byte, as shipped atoms do.
// The client indexes the target's selector array
inline constexpr ValueSpec kAbilityPointArgument{.name = "value",
                                                 .kind = ValueKind::integer,
                                                 .optional = true,
                                                 .fallback = default_integer(0),
                                                 .bound = between(0, 127),
                                                 .scope = Scope::argument,
                                                 .note = "authored point parameter of target"};
// Which entity the ability is aimed at. The client copies the lane into the actor's ability
// request and resolves it the same way for every actor:
//   -1  no entity; the ability uses the position of target/value alone (a teleport)
//    0  the actor's primary combat target, which its own target selection keeps up to date (player)
//    1  target slot 1, set from outside the AI (a type-40 target sensor or a scripted target)
//    2  the highest-scoring perceived target of the secondary target class
//    3  the actor linked to this one in its AI record
//    4  the actor itself
inline constexpr ValueSpec kAbilityModeArgument{.name = "mode",
                                                .kind = ValueKind::integer,
                                                .optional = true,
                                                .fallback = default_integer(-1),
                                                .bound = between(-1, 4),
                                                .scope = Scope::argument,
                                                .note = "what the ability is aimed at: -1 the "
                                                        "point only, 0..2 a target slot, 3 the "
                                                        "linked actor, 4 itself"};
inline constexpr ValueSpec kAtomAbilityFields[]{kAtomKindField,
                                                kAbilityArgument,
                                                kAbilityTargetArgument,
                                                kAbilityPointArgument,
                                                kAbilityModeArgument,
                                                kAtomQuantizedField};

/** The ten primary lane schemas the client-atom runner selects between, in native order. */
inline constexpr std::array<AtomKindSpec, 10> kAtomKinds{{
    {.name = "face", .fields = kAtomPointFields, .note = "turn to a point of target"},
    {.name = "sequence", .fields = kAtomSequenceFields, .note = "play a sequence by number"},
    {.name = "sleep", .fields = kAtomSleepFields, .note = "wait"},
    {.name = "move_to", .fields = kAtomMoveFields, .note = "move to a point of target"},
    {.name = "trivial", .fields = kAtomTrivialFields, .note = "an empty step"},
    {.name = "control_flag", .fields = kAtomControlFlagFields, .note = "set a control flag"},
    {.name = "set_temperament", .fields = kAtomTemperamentFields, .note = "set a temperament"},
    {.name = "set_channel", .fields = kAtomChannelFields, .note = "set an actor channel"},
    {.name = "snap_to", .fields = kAtomPointFields, .note = "jump to a point of target"},
    {.name = "ability",
     .fields = kAtomAbilityFields,
     .closed = true,
     .note = "use an ability, aimed by target/value and mode"},
}};

/** @return The field set of one atom kind. Fails compilation for an unknown kind. */
[[nodiscard]] consteval const AtomKindSpec& find_atom_kind(std::string_view name) {
    for (const AtomKindSpec& kind : kAtomKinds) {
        if (kind.name == name) {
            return kind;
        }
    }
    throw "atom kind is not declared";
}

// --- The context.sdk collections ---------------------------------------------------------------

// Atom kind names in native order; each Lua entry maps the name to itself.
inline constexpr std::array<EnumEntry, kAtomKinds.size()> kAtomKindEntries = [] {
    std::array<EnumEntry, kAtomKinds.size()> entries{};
    for (std::size_t index = 0; index < kAtomKinds.size(); ++index) {
        entries[index] = {.name = kAtomKinds[index].name,
                          .value = static_cast<std::int64_t>(index)};
    }
    return entries;
}();

// Lane names with their capacity as the value and their element kind as the detail.
inline constexpr std::array<EnumEntry, kBoundedLanes.size()> kBoundedLaneEntries = [] {
    std::array<EnumEntry, kBoundedLanes.size()> entries{};
    for (std::size_t index = 0; index < kBoundedLanes.size(); ++index) {
        entries[index] = {.name = kBoundedLanes[index].name,
                          .value = kBoundedLanes[index].capacity,
                          .detail = bounded_lane_kind_name(kBoundedLanes[index].kind)};
    }
    return entries;
}();

// Every closed collection under context.sdk, in schema order.
inline constexpr EnumSpec kEnums[]{
    {.name = "device_channels",
     .luaClass = "SunriseDeviceChannel",
     .entries = kDeviceChannelEntries,
     .note = "value is the type-23 wire lane"},
    {.name = "device_transitions",
     .luaClass = "SunriseDeviceTransition",
     .entries = kDeviceTransitionEntries,
     .detailName = "channel",
     .note = "value is the endpoint the channel moves to: 0 low, 1 high"},
    {.name = "squad_modes",
     .luaClass = "SunriseSquadMode",
     .entries = kSquadModeEntries,
     .note = "value is the squad Auth placement mode"},
    {.name = "lifetime_states",
     .luaClass = "SunriseLifetimeState",
     .range = between(0, kMaximumLifetimeState),
     .hasDefault = true,
     .defaultValue = kDefaultLifetimeState,
     .note = "reached by at(n); complete_mission sets state 6"},
    {.name = "atom_kinds",
     .luaClass = "string",
     .entries = kAtomKindEntries,
     .note = "each Lua entry is its own name; value is the native lane order"},
    {.name = "bounded_lanes",
     .luaClass = "SunriseBoundedLane",
     .entries = kBoundedLaneEntries,
     .valueName = "capacity",
     .detailName = "kind",
     .note = "value is the client array size"},
};

} // namespace sunrise::server::activity::mission::api
