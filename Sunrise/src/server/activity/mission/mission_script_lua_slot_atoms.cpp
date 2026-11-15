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
/** A positive template counter for the encoder; the Host assigns every transported one. */
constexpr std::uint32_t kUncommittedRevision = 1;
constexpr const api::MemberSpec& kActorControls = api::slot_member("set_actor_controls");
constexpr const char* kTemperamentsKey =
    optional_key(kActorControls, "temperaments", api::ValueKind::list);
constexpr const char* kChannelsKey = optional_key(kActorControls, "channels", api::ValueKind::list);
// run_atoms takes the same two lists, so one reader serves both.
static_assert(std::string_view(optional_key(kRunAtoms, "temperaments", api::ValueKind::list))
                  == kTemperamentsKey
              && std::string_view(optional_key(kRunAtoms, "channels", api::ValueKind::list))
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
                                     std::uint32_t slotRow,
                                     scriptable_auth::Type2ChannelState& controls) {
    std::array<std::uint32_t, scriptable_auth::kType2TemperamentCapacity> ids{};
    std::uint8_t count = 0;
    if (!read_temperament_list(state, kTemperamentsKey, slotRow, ids, count)) {
        return false;
    }
    std::array<scriptable_auth::TemperamentId, scriptable_auth::kType2TemperamentCapacity>
        temperaments{};
    for (std::size_t index = 0; index < count; ++index) {
        temperaments[index].value = ids[index];
    }
    if (!scriptable_auth::set_type2_temperaments(controls, std::span(temperaments).first(count))) {
        static_cast<void>(luaL_error(state, "temperaments must be distinct"));
    }
    return true;
}

/** Reads the channels list into the controls. @return False when the call gave none. */
[[nodiscard]] bool read_channels(lua_State* state,
                                 std::uint32_t slotRow,
                                 scriptable_auth::Type2ChannelState& controls) {
    constexpr std::span<const api::ValueSpec> kRowFields =
        api::find_field(kChannelFields, "channel").fields;
    constexpr const char* kOwnerKey = api::find_field(kRowFields, "slot_row").name.data();
    constexpr const char* kNameKey = api::find_field(kRowFields, "channel").name.data();
    const Impl* const impl = impl_from_state(state);
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
        lua_getfield(state, row, kChannelKey);
        luaL_checktype(state, -1, LUA_TTABLE);
        const int channel = lua_gettop(state);
        const lua_Integer owner = directive_integer(state, channel, kOwnerKey);
        const std::uint32_t name = atom_u32(state, channel, kNameKey);
        lua_pop(state, 1);
        if (owner != static_cast<lua_Integer>(slotRow)
            || impl->definitions.resolveActorChannel == nullptr
            || !impl->definitions.resolveActorChannel(impl->definitions.context, slotRow, name)) {
            static_cast<void>(
                luaL_error(state, "channels must name mission.ActorChannel rows of this actor"));
        }
        const auto held = std::span(controls.channels).first(controls.count);
        if (std::any_of(held.begin(), held.end(), [name](const auto& entry) noexcept {
                return entry.nameHash == name;
            })) {
            static_cast<void>(luaL_error(state, "channels must be distinct"));
        }
        if (!scriptable_auth::set_type2_channel(
                controls, name, atom_real(state, row, kChannelValueKey))) {
            static_cast<void>(luaL_error(state,
                                         "an actor holds at most %d channels",
                                         static_cast<int>(controls.channels.size())));
        }
        lua_pop(state, 1);
    }
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
    scriptable_auth::Type2Body body{};
    body.channels.revision = kUncommittedRevision;
    body.atoms.generation = kUncommittedRevision;
    const bool temperaments = read_temperaments(state, slot.nativeRow, body.channels);
    const bool channels = read_channels(state, slot.nativeRow, body.channels);
    if ((temperaments || channels)
        && (!spawn || (body.channels.temperamentCount == 0 && body.channels.count == 0))) {
        return luaL_error(state, "initial actor controls need spawn and a temperament or channel");
    }
    lua_getfield(state, 2, required_key(kRunAtoms, "atoms", api::ValueKind::list));
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const auto count = static_cast<std::size_t>(lua_rawlen(state, list));
    if (count > body.atoms.lanes.size()) {
        return luaL_error(state, "an atom program holds up to 32 lanes");
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

/** Stages a complete control set; the Host writes it under the actor's next control revision. */
[[nodiscard]] int slot_set_actor_controls(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    refuse_unknown_arguments(state, kActorControls);
    scriptable_auth::Type2Body body{};
    body.channels.revision = kUncommittedRevision;
    static_cast<void>(read_temperaments(state, slot.nativeRow, body.channels));
    static_cast<void>(read_channels(state, slot.nativeRow, body.channels));
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
