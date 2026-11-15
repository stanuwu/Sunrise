#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/auth_fields.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "mission_script_carrier_plan.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;

namespace {

/** @return One field of one atom kind. Fails compilation when the table lacks it. */
[[nodiscard]] consteval const api::ValueSpec& atom_field(std::string_view kind,
                                                         std::string_view name) {
    return api::find_field(api::find_atom_kind(kind).fields, name);
}

/** @return The key of one field of one atom kind. */
[[nodiscard]] consteval const char* atom_key(std::string_view kind, std::string_view name) {
    return atom_field(kind, name).name.data();
}

/** Reads one unsigned 32-bit field from an atom declaration. */
[[nodiscard]] std::uint32_t atom_u32(lua_State* state, int table, const char* field) {
    const lua_Integer value = directive_integer(state, table, field);
    if (value < 0 || value > (std::numeric_limits<std::uint32_t>::max)()) {
        static_cast<void>(luaL_error(state, "atom field '%s' is outside a 32-bit range", field));
    }
    return static_cast<std::uint32_t>(value);
}

/** Reads one finite real field from an atom declaration. */
[[nodiscard]] float atom_real(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const auto value = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    if (!std::isfinite(value)) {
        static_cast<void>(luaL_error(state, "atom field '%s' is not finite", field));
    }
    return value;
}

/** Reads one optional small unsigned field from an atom declaration. */
[[nodiscard]] std::uint8_t
atom_u8(lua_State* state, int table, const char* field, std::uint8_t bound) {
    lua_getfield(state, table, field);
    const lua_Integer value = lua_isnil(state, -1) ? 0 : luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (value < 0 || value > bound) {
        static_cast<void>(luaL_error(state, "atom field '%s' is outside its native width", field));
    }
    return static_cast<std::uint8_t>(value);
}

/** Reads one optional boolean field from an atom declaration. */
[[nodiscard]] bool atom_flag(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

/** Resolves the live Slot handle an atom points at into its exact 55-bit client reference. */
[[nodiscard]] scriptable_auth::Type2LaneClientRef atom_target(lua_State* state, int table) {
    scriptable_auth::Type2LaneClientRef reference{};
    lua_getfield(state, table, api::kAtomTargetField.name.data());
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
    SlotDefinition target{};
    if (!current_slot(state, *handle, target)) {
        static_cast<void>(luaL_error(state, "atom target slot is stale"));
    }
    // A client reference carries the slot index as a signed 16-bit lane.
    constexpr auto kHighestReferenceIndex =
        static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)());
    if (target.slotType > 126
        || static_cast<std::uint32_t>(target.slotIndex) > kHighestReferenceIndex) {
        static_cast<void>(luaL_error(state, "atom target slot is outside a client reference"));
    }
    reference.registryKey = target.registryKey;
    reference.slotType = static_cast<std::int8_t>(target.slotType);
    reference.slotIndex = static_cast<std::int16_t>(target.slotIndex);
    lua_pop(state, 1);
    return reference;
}

/** The point reader has no bounds check, so every parameter must belong to the SDK target. */
[[nodiscard]] std::uint8_t atom_point_parameter(lua_State* state, int table) {
    const auto parameter = atom_u8(state,
                                   table,
                                   api::kAtomPointField.name.data(),
                                   static_cast<std::uint8_t>(api::kAtomPointField.bound.maximum));
    lua_getfield(state, table, api::kAtomTargetField.name.data());
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
    SlotDefinition target{};
    const bool current = current_slot(state, *handle, target);
    lua_pop(state, 1);
    const Impl* impl = impl_from_state(state);
    if (!current || impl->definitions.resolveActorAbilityTarget == nullptr
        || !impl->definitions.resolveActorAbilityTarget(
            impl->definitions.context, target.nativeRow, parameter)) {
        static_cast<void>(luaL_error(state, "atom target has no such authored point parameter"));
    }
    return parameter;
}

scriptable_auth::Type2LanePrimary atom_face(lua_State* state, int table) {
    return scriptable_auth::Type2LaneAlternateRefByte{atom_target(state, table),
                                                      atom_point_parameter(state, table)};
}

scriptable_auth::Type2LanePrimary atom_snap_to(lua_State* state, int table) {
    return scriptable_auth::Type2LaneRefByte{atom_target(state, table),
                                             atom_point_parameter(state, table)};
}

scriptable_auth::Type2LanePrimary atom_sequence(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32{atom_u32(state, table, atom_key("sequence", "value"))};
}

scriptable_auth::Type2LanePrimary atom_sleep(lua_State* state, int table) {
    return scriptable_auth::Type2LaneReal32{atom_real(state, table, atom_key("sleep", "seconds"))};
}

scriptable_auth::Type2LanePrimary atom_move_to(lua_State* state, int table) {
    return scriptable_auth::Type2LaneRefByteBool{
        atom_target(state, table),
        atom_point_parameter(state, table),
        atom_flag(state, table, atom_key("move_to", "enabled"))};
}

/** @return The key of one field of a generated mission.ActorAbility row. */
[[nodiscard]] consteval const char* ability_row_key(std::string_view name) {
    return api::find_field(atom_field("ability", "ability").fields, name).name.data();
}

scriptable_auth::Type2LanePrimary atom_trivial(lua_State* /*state*/, int /*table*/) {
    return scriptable_auth::Type2LaneEmpty{};
}

scriptable_auth::Type2LanePrimary atom_control_flag(lua_State* state, int table) {
    // The flag lane is six bits wide.
    constexpr const api::ValueSpec& kFlag = atom_field("control_flag", "value");
    return scriptable_auth::Type2LaneU6{
        atom_u8(state, table, kFlag.name.data(), static_cast<std::uint8_t>(kFlag.bound.maximum))};
}

scriptable_auth::Type2LanePrimary atom_set_temperament(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32Bool{
        atom_u32(state, table, atom_key("set_temperament", "value")),
        atom_flag(state, table, atom_key("set_temperament", "enabled"))};
}

scriptable_auth::Type2LanePrimary atom_set_channel(lua_State* state, int table) {
    return scriptable_auth::Type2LaneU32Real32{
        atom_u32(state, table, atom_key("set_channel", "channel")),
        atom_real(state, table, atom_key("set_channel", "value"))};
}

/**
 * Reads one generated mission.ActorAbility row at stack index `row` into an untargeted ability
 * lane; the ability must belong to the actor.
 */
[[nodiscard]] scriptable_auth::Type2LaneTripleRef
actor_ability(lua_State* state, int row, const SlotDefinition& actor) {
    luaL_checktype(state, row, LUA_TTABLE);
    const std::uint32_t owner = atom_u32(state, row, ability_row_key("slot_row"));
    const std::uint32_t group = atom_u32(state, row, ability_row_key("group_hash"));
    const std::uint32_t request = atom_u32(state, row, ability_row_key("request_hash"));
    Impl* const impl = impl_from_state(state);
    if (owner != actor.nativeRow || impl == nullptr
        || impl->definitions.resolveActorAbility == nullptr
        || !impl->definitions.resolveActorAbility(
            impl->definitions.context, owner, group, request)) {
        static_cast<void>(luaL_error(state, "ability does not belong to this SDK actor"));
    }
    scriptable_auth::Type2LaneTripleRef ability{};
    ability.values = {
        group, request, middleware::bap::activity_message::auth_fields::kClientRefAbsentKey};
    ability.reference.slotType = -1;
    ability.mode = -1;
    ability.value = -1;
    return ability;
}

/** Uses one exact actor-owned ability and the first parameter of an optional authored point. */
scriptable_auth::Type2LanePrimary
atom_ability(lua_State* state, int table, const SlotDefinition& actor) {
    table = lua_absindex(state, table);
    // Actor-target modes and raw hash tuples have no script-level meaning on this path.
    constexpr const api::AtomKindSpec& kAbility = api::find_atom_kind("ability");
    static_assert(kAbility.closed);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        const auto key = lua_string_view(state, -2);
        if (std::none_of(kAbility.fields.begin(),
                         kAbility.fields.end(),
                         [key](const api::ValueSpec& field) { return field.name == key; })) {
            static_cast<void>(luaL_error(state, "unknown ability atom field"));
        }
        lua_pop(state, 1);
    }
    lua_getfield(state, table, api::find_field(kAbility.fields, "ability").name.data());
    scriptable_auth::Type2LaneTripleRef ability = actor_ability(state, lua_gettop(state), actor);
    lua_pop(state, 1);
    Impl* const impl = impl_from_state(state);
    const char* const targetKey = api::find_field(kAbility.fields, "target").name.data();
    lua_getfield(state, table, targetKey);
    const bool targeted = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (targeted) {
        lua_getfield(state, table, targetKey);
        const auto* const handle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition target{};
        const bool current = current_slot(state, *handle, target);
        lua_pop(state, 1);
        // Selector zero is the first authored point parameter, not an actor-target mode.
        constexpr std::uint32_t kFirstPointParameter = 0;
        if (!current || impl->definitions.resolveActorAbilityTarget == nullptr
            || !impl->definitions.resolveActorAbilityTarget(
                impl->definitions.context, target.nativeRow, kFirstPointParameter)) {
            static_cast<void>(luaL_error(state, "ability target has no authored point parameter"));
        }
        ability.reference = atom_target(state, table);
        ability.value = static_cast<std::int8_t>(kFirstPointParameter);
    }
    return ability;
}
/** One script-facing atom name and the reader that fills its native lane child. */
struct AtomKind final {
    std::string_view name;
    scriptable_auth::Type2LanePrimary (*read)(lua_State*, int);
};

/** The ten primary lane schemas the client-atom runner selects between, in table order. */
constexpr std::array<AtomKind, api::kAtomKinds.size()> kAtomKinds{{
    {"face", &atom_face},
    {"sequence", &atom_sequence},
    {"sleep", &atom_sleep},
    {"move_to", &atom_move_to},
    {"trivial", &atom_trivial},
    {"control_flag", &atom_control_flag},
    {"set_temperament", &atom_set_temperament},
    {"set_channel", &atom_set_channel},
    {"snap_to", &atom_snap_to},
    {"ability", nullptr},
}};
static_assert([] {
    for (std::size_t index = 0; index < kAtomKinds.size(); ++index) {
        if (kAtomKinds[index].name != api::kAtomKinds[index].name) {
            return false;
        }
    }
    return true;
}());

// Table entries of the members this file implements.
constexpr const api::MemberSpec& kRunAtoms = api::slot_member("run_atoms");
constexpr const api::MemberSpec& kActorControls = api::slot_member("set_actor_controls");
constexpr const char* kTemperamentsKey =
    optional_key(kActorControls, "temperaments", api::ValueKind::list);
constexpr const char* kChannelsKey = optional_key(kActorControls, "channels", api::ValueKind::list);
// run_atoms takes the same two lists, so one reader serves both.
static_assert(std::string_view(optional_key(kRunAtoms, "temperaments", api::ValueKind::list))
                  == kTemperamentsKey
              && std::string_view(optional_key(kRunAtoms, "channels", api::ValueKind::list))
                     == kChannelsKey);
constexpr const api::MemberSpec& kStartCarrier = api::slot_member("start_carrier");
constexpr const api::MemberSpec& kCancelCarrier = api::slot_member("cancel_carrier");
// start_carrier reads its temperaments and channels with the same readers.
static_assert(std::string_view(optional_key(kStartCarrier, "temperaments", api::ValueKind::list))
                  == kTemperamentsKey
              && std::string_view(optional_key(kStartCarrier, "channels", api::ValueKind::list))
                     == kChannelsKey);
// The fields of one channel row.
constexpr std::span<const api::ValueSpec> kChannelFields =
    api::find_argument(kActorControls, "channels").element.front().fields;
constexpr const char* kChannelKey = api::find_field(kChannelFields, "channel").name.data();
constexpr const char* kChannelValueKey = api::find_field(kChannelFields, "value").name.data();

/** Builds one atom lane from its declaration table at stack index `table`. */
void parse_atom(lua_State* state,
                int table,
                const SlotDefinition& actor,
                scriptable_auth::Type2KeyedLane& lane) {
    lua_getfield(state, table, api::kAtomKindField.name.data());
    const std::string_view kind = lua_string_view(state, -1);
    const auto match =
        std::find_if(kAtomKinds.begin(), kAtomKinds.end(), [kind](const AtomKind& row) noexcept {
            return row.name == kind;
        });
    if (match == kAtomKinds.end()) {
        static_cast<void>(luaL_error(state, "unknown atom kind"));
        return;
    }
    lane.primary =
        match->read != nullptr ? match->read(state, table) : atom_ability(state, table, actor);
    lua_pop(state, 1);
    lua_getfield(state, table, api::kAtomQuantizedField.name.data());
    if (!lua_isnil(state, -1)) {
        const lua_Integer quantized = luaL_checkinteger(state, -1);
        if (quantized < 0 || quantized > api::kAtomQuantizedField.bound.maximum) {
            static_cast<void>(luaL_error(state, "atom quantized value is wider than 11 bits"));
        }
        lane.secondary =
            scriptable_auth::Type2LaneQuantized11{static_cast<std::uint16_t>(quantized)};
    }
    lua_pop(state, 1);
}

/** Reads the temperaments list into the controls. @return False when the call gave none. */
[[nodiscard]] bool read_temperaments(lua_State* state,
                                     scriptable_auth::Type2ChannelState& controls) {
    if (push_argument(state, kTemperamentsKey) == LUA_TNIL) {
        lua_pop(state, 1);
        return false;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    std::array<scriptable_auth::TemperamentId, scriptable_auth::kType2TemperamentCapacity>
        temperaments{};
    const std::size_t count = lua_rawlen(state, list);
    if (count > temperaments.size()) {
        static_cast<void>(luaL_error(state,
                                     "an actor holds at most %d temperaments",
                                     static_cast<int>(temperaments.size())));
    }
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const lua_Integer value = luaL_checkinteger(state, -1);
        if (value < 0 || value > (std::numeric_limits<std::uint32_t>::max)()) {
            static_cast<void>(luaL_error(state, "temperament is outside a 32-bit range"));
        }
        temperaments[index].value = static_cast<std::uint32_t>(value);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    if (!scriptable_auth::set_type2_temperaments(controls, std::span(temperaments).first(count))) {
        static_cast<void>(luaL_error(state, "temperaments must be distinct"));
    }
    return true;
}

/** Reads the channels list into the controls. @return False when the call gave none. */
[[nodiscard]] bool read_channels(lua_State* state, scriptable_auth::Type2ChannelState& controls) {
    if (push_argument(state, kChannelsKey) == LUA_TNIL) {
        lua_pop(state, 1);
        return false;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        luaL_checktype(state, -1, LUA_TTABLE);
        const int row = lua_gettop(state);
        refuse_unknown_fields(state, row, kChannelFields);
        if (!scriptable_auth::set_type2_channel(controls,
                                                atom_u32(state, row, kChannelKey),
                                                atom_real(state, row, kChannelValueKey))) {
            static_cast<void>(luaL_error(state,
                                         "an actor holds at most %d channels",
                                         static_cast<int>(controls.channels.size())));
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

/** Reads an authored path of the actor's own object and counts the points one program can fly. */
[[nodiscard]] CarrierPath carrier_path(lua_State* state,
                                       const char* key,
                                       const SlotDefinition& actor) {
    const auto handle = checked_argument<SlotHandle>(state, key, kSlotMetatable);
    SlotDefinition path{};
    const Impl* const impl = impl_from_state(state);
    if (!current_slot(state, handle, path) || !exact_actor_path(path, actor)
        || impl->definitions.resolveActorAbilityTarget == nullptr) {
        static_cast<void>(luaL_error(state, "%s must be an exact authored path of the actor", key));
    }
    // A path is one point parameter per point; one program moves through at most every lane.
    std::size_t points = 0;
    while (points <= scriptable_auth::kType2AtomCapacity
           && impl->definitions.resolveActorAbilityTarget(
               impl->definitions.context, path.nativeRow, static_cast<std::uint32_t>(points))) {
        ++points;
    }
    if (points == 0 || points > scriptable_auth::kType2AtomCapacity) {
        static_cast<void>(luaL_error(state, "%s has no point or more than one program holds", key));
    }
    return {{path.registryKey,
             static_cast<std::int8_t>(path.slotType),
             static_cast<std::int16_t>(path.slotIndex)},
            static_cast<std::uint8_t>(points)};
}

/** @return The runnable squad an exact authored squad slot of the actor's own object holds. */
[[nodiscard]] SquadDefinition cargo_squad(lua_State* state,
                                          const SlotDefinition& slot,
                                          const SlotDefinition& actor) {
    const Impl* const impl = impl_from_state(state);
    if (slot.slotType != format::kSquadSlotType
        || slot.componentClass != format::kSquadComponentClass
        || slot.authSchema != middleware::bap::activity_message::squad_auth::kSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0 || slot.objectTag != actor.objectTag
        || slot.registryKey != actor.registryKey
        || slot.slotIndex > middleware::bap::activity_message::auth_fields::kMaximumClientRefIndex
        || impl->definitions.squadCount == nullptr || impl->definitions.resolveSquadRow == nullptr) {
        static_cast<void>(luaL_error(state, "cargo must be exact squads of the actor's object"));
    }
    // Squad rows are one-based within the bound scenario.
    const std::size_t count = impl->definitions.squadCount(impl->definitions.context);
    for (std::size_t row = 1; row <= count; ++row) {
        SquadDefinition squad{};
        if (impl->definitions.resolveSquadRow(
                impl->definitions.context, static_cast<std::uint32_t>(row), squad)
            && squad.slotRow == slot.nativeRow) {
            return squad;
        }
    }
    static_cast<void>(luaL_error(state, "cargo slot holds no runnable squad"));
    return {};
}

/** Reads the cargo list: distinct squads, each with members, each with one when serial. */
void read_cargo(lua_State* state, const SlotDefinition& actor, CarrierDefinition& definition) {
    lua_getfield(state, kArgumentTable, required_key(kStartCarrier, "cargo", api::ValueKind::list));
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    if (count > definition.cargo.size()) {
        static_cast<void>(luaL_error(state,
                                     "one carrier unloads at most %d squads",
                                     static_cast<int>(definition.cargo.size())));
    }
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const auto* const handle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition slot{};
        if (!current_slot(state, *handle, slot)) {
            static_cast<void>(luaL_error(state, "cargo slot is stale"));
        }
        lua_pop(state, 1);
        const SquadDefinition squad = cargo_squad(state, slot, actor);
        CarrierCargo& cargo = definition.cargo[index];
        std::int64_t members = 0;
        for (std::size_t member = 0; member < squad.memberCount; ++member) {
            members += squad.defaultCounts[member];
        }
        const bool repeated =
            std::any_of(definition.cargo.begin(),
                        definition.cargo.begin() + static_cast<std::ptrdiff_t>(index),
                        [&squad](const CarrierCargo& prior) {
                            return prior.squadRow == squad.nativeRow;
                        });
        if (repeated || members <= 0 || (definition.serial && members != 1)) {
            static_cast<void>(luaL_error(
                state, "cargo squads must be distinct with members, one each when serial"));
        }
        cargo.counts = squad.defaultCounts;
        cargo.memberCount = static_cast<std::uint8_t>(squad.memberCount);
        cargo.squadRow = squad.nativeRow;
        cargo.reference = {slot.registryKey, static_cast<std::uint16_t>(slot.slotIndex)};
    }
    lua_pop(state, 1);
    definition.cargoCount = static_cast<std::uint8_t>(count);
}

/** @return The entry mode its script spelling names. */
[[nodiscard]] CarrierEntryMode carrier_entry_mode(lua_State* state) {
    const char* const key = optional_key(kStartCarrier, "entry_mode", api::ValueKind::string);
    if (push_argument(state, key) == LUA_TNIL) {
        lua_pop(state, 1);
        return CarrierEntryMode::path;
    }
    const std::string_view mode = luaL_checkstring(state, -1);
    lua_pop(state, 1);
    if (mode == "path") {
        return CarrierEntryMode::path;
    }
    if (mode == "ability") {
        return CarrierEntryMode::ability;
    }
    if (mode == "ability_then_path") {
        return CarrierEntryMode::abilityThenPath;
    }
    if (mode == "path_then_ability") {
        return CarrierEntryMode::pathThenAbility;
    }
    static_cast<void>(luaL_error(state, "unknown carrier entry_mode"));
    return CarrierEntryMode::path;
}

/** Reads one ability argument of the actor; @return False when an optional one is absent. */
[[nodiscard]] bool carrier_ability(lua_State* state,
                                   const char* key,
                                   const SlotDefinition& actor,
                                   scriptable_auth::Type2LaneTripleRef& output) {
    if (push_argument(state, key) == LUA_TNIL) {
        lua_pop(state, 1);
        return false;
    }
    output = actor_ability(state, lua_gettop(state), actor);
    lua_pop(state, 1);
    return true;
}
} // namespace

/** Exposes the same closed operation names the native atom parser accepts. */
void push_atom_kinds(lua_State* state) {
    lua_createtable(state, 0, static_cast<int>(kAtomKinds.size()));
    for (const auto& kind : kAtomKinds) {
        lua_pushlstring(state, kind.name.data(), kind.name.size());
        lua_setfield(state, -2, kind.name.data());
    }
}

/** Stages an actor program while native state owns its creation and program revisions. */
[[nodiscard]] int slot_run_atoms(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    refuse_unknown_arguments(state, kRunAtoms);
    const bool spawn = optional_boolean_argument(state, boolean_option(kRunAtoms, "spawn"));
    // The encoder needs a positive template value; only the native reducer publishes counters.
    constexpr std::uint32_t kUncommittedRevision = 1;
    scriptable_auth::Type2Body body{};
    body.channels.revision = kUncommittedRevision;
    body.atoms.generation = kUncommittedRevision;
    const bool temperaments = read_temperaments(state, body.channels);
    const bool channels = read_channels(state, body.channels);
    if ((temperaments || channels)
        && (!spawn || (body.channels.temperamentCount == 0 && body.channels.count == 0))) {
        return luaL_error(state, "initial actor controls need spawn and a temperament or channel");
    }
    lua_getfield(state, 2, required_key(kRunAtoms, "atoms", api::ValueKind::list));
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const auto count = static_cast<std::size_t>(lua_rawlen(state, list));
    if ((count == 0 && !spawn) || count > body.atoms.lanes.size()) {
        return luaL_error(state, "an atom program holds one to 32 lanes, or none when it spawns");
    }
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index) + 1);
        luaL_checktype(state, -1, LUA_TTABLE);
        parse_atom(state, lua_gettop(state), slot, body.atoms.lanes[index]);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    body.atoms.count = static_cast<std::uint8_t>(count);

    std::array<std::byte, scriptable_auth::kType2MaximumBodyByteCount> encoded{};
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    if (!scriptable_auth::encode_type2_body(body, encoded, written, writtenBits)) {
        return luaL_error(state, "atom program native encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           scriptable_auth::kType2Schema,
                           writtenBits,
                           std::span<const std::byte>(encoded.data(), written),
                           IntentKind::runActorProgram,
                           spawn);
}

/**
 * Opens one carrier flight. The script names authored paths, abilities and cargo; State runs the
 * compiled steps and the Host owns every counter.
 */
[[nodiscard]] int slot_start_carrier(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kStartCarrier);
    SlotDefinition actor{};
    CarrierDefinition definition{};
    Impl* const impl = impl_from_state(state);
    std::uint32_t parentSquadRow = 0;
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || impl->definitions.resolveActorProgramSource == nullptr
        || !impl->definitions.resolveActorProgramSource(
            impl->definitions.context, actor.nativeRow, parentSquadRow)
        || !decode_sdk_build_sha256(std::string_view(impl->identity.sdkBuildId.data()),
                                    definition.actor.sdkBuildSha256)) {
        return luaL_error(state, "a carrier needs an exact actor with one authored source");
    }
    definition.actor.firstRow = actor.nativeRow;
    definition.actor.secondRow = parentSquadRow;
    definition.actor.objectTag = actor.objectTag;
    definition.actor.registryKey = actor.registryKey;
    definition.actor.authSchema = combatant::kSchema;
    definition.actor.slotIndex = static_cast<std::uint16_t>(actor.slotIndex);
    definition.actor.slotType = static_cast<std::uint8_t>(actor.slotType);
    using Kind = api::ValueKind;
    definition.entry =
        carrier_path(state, required_key(kStartCarrier, "entry", Kind::handle), actor);
    definition.exit = carrier_path(state, required_key(kStartCarrier, "exit", Kind::handle), actor);
    definition.entryMode = carrier_entry_mode(state);
    const bool entryAbility =
        carrier_ability(state,
                        optional_key(kStartCarrier, "entry_ability", Kind::missionRow),
                        actor,
                        definition.entryAbility);
    if (entryAbility == (definition.entryMode == CarrierEntryMode::path)) {
        return luaL_error(state, "entry_ability goes with every entry_mode but path");
    }
    if (!carrier_ability(state,
                         required_key(kStartCarrier, "exit_ability", Kind::missionRow),
                         actor,
                         definition.exitAbility)) {
        return luaL_error(state, "exit_ability is required");
    }
    definition.relocateStart =
        optional_boolean_argument(state, boolean_option(kStartCarrier, "relocate_start"));
    definition.serial = optional_boolean_argument(state, boolean_option(kStartCarrier, "serial"));
    const lua_Integer door =
        checked_integer_argument(state, required_key(kStartCarrier, "door_channel", Kind::integer));
    if (door < 0 || door > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "door_channel is outside a 32-bit range");
    }
    definition.doorChannel = static_cast<std::uint32_t>(door);
    const char* const markerKey = optional_key(kStartCarrier, "exit_marker", Kind::integer);
    if (push_argument(state, markerKey) != LUA_TNIL) {
        const lua_Integer marker = luaL_checkinteger(state, -1);
        if (marker < 0 || marker >= definition.exit.points) {
            return luaL_error(state, "exit_marker is not a point of the exit path");
        }
        definition.exitMarker = static_cast<std::int16_t>(marker);
    }
    lua_pop(state, 1);
    read_cargo(state, actor, definition);
    static_cast<void>(read_temperaments(state, definition.controls));
    static_cast<void>(read_channels(state, definition.controls));
    Intent intent{};
    intent.kind = IntentKind::startCarrier;
    intent.firstRow = actor.nativeRow;
    try {
        intent.carrierPlan = compile_carrier_plan(definition);
    } catch (const std::bad_alloc&) {
        active_frame(state).intentAllocationFailed = true;
        return luaL_error(state, "carrier plan allocation failed");
    }
    if (intent.carrierPlan == nullptr) {
        return luaL_error(state, "carrier flight does not fit its native actor bodies");
    }
    return queue_intent(state, active_frame(state), intent);
}

/** Ends the actor's flight; State drops its queued step and runs no further one. */
[[nodiscard]] int slot_cancel_carrier(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kCancelCarrier);
    SlotDefinition actor{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    Intent intent{};
    intent.kind = IntentKind::cancelCarrier;
    intent.firstRow = actor.nativeRow;
    return queue_intent(state, active_frame(state), intent);
}

/** Stages a complete control set; the Host writes it under the actor's next control revision. */
[[nodiscard]] int slot_set_actor_controls(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    refuse_unknown_arguments(state, kActorControls);
    // The encoder needs a positive template value; the Host assigns the transported revision.
    constexpr std::uint32_t kUncommittedRevision = 1;
    scriptable_auth::Type2Body body{};
    body.channels.revision = kUncommittedRevision;
    static_cast<void>(read_temperaments(state, body.channels));
    static_cast<void>(read_channels(state, body.channels));
    std::array<std::byte, scriptable_auth::kType2MaximumBodyByteCount> encoded{};
    std::size_t written = 0;
    std::size_t writtenBits = 0;
    if (!scriptable_auth::encode_type2_body(body, encoded, written, writtenBits)) {
        return luaL_error(state, "actor control native encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           scriptable_auth::kType2Schema,
                           writtenBits,
                           std::span<const std::byte>(encoded.data(), written),
                           IntentKind::setActorControls);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
