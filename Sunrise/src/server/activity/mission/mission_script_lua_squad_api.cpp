#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_lua_world_internal.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Table entries of the squad members this file implements.
constexpr const api::MemberSpec& kPlace = api::member(api::Receiver::squad, "place");
constexpr const api::MemberSpec& kActorCommand = api::member(api::Receiver::squad, "actor_command");

/** Squad members in table order. */
constexpr std::array<Binding<SquadDefinition>, std::size(api::kSquadMembers)> kSquadBindings{{
    {"row",
     nullptr,
     [](lua_State* state, const SquadDefinition& squad) {
         lua_pushinteger(state, squad.localRow);
     }},
    {"id",
     nullptr,
     [](lua_State* state, const SquadDefinition& squad) {
         lua_pushlstring(state, squad.id.data(), squad.id.size());
     }},
    {"name",
     nullptr,
     [](lua_State* state, const SquadDefinition& squad) {
         lua_pushlstring(state, squad.name.data(), squad.name.size());
     }},
    {"member_count",
     nullptr,
     [](lua_State* state, const SquadDefinition& squad) {
         lua_pushinteger(state, static_cast<lua_Integer>(squad.memberCount));
     }},
    {"default_counts",
     nullptr,
     [](lua_State* state, const SquadDefinition& squad) {
         lua_createtable(state, static_cast<int>(squad.memberCount), 0);
         for (std::size_t index = 0; index < squad.memberCount; ++index) {
             lua_pushinteger(state, squad.defaultCounts[index]);
             lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
         }
     }},
    {"counts", &squad_counts},
    {"anchors", nullptr, &world_api::push_squad_anchors},
    {"place", &squad_place},
    {"actor_command", &squad_actor_command},
}};
static_assert(bound_to<SquadDefinition>(api::kSquadMembers, kSquadBindings));

} // namespace

/** Lua index for a squad handle: its named fields and collections. */
[[nodiscard]] int squad_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    return dispatch<SquadDefinition>(state, kSquadBindings, lua_string_view(state, 2), definition);
}

/** Stages one SDK-selected scalar actor command for every live member of a squad. */
[[nodiscard]] int squad_actor_command(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition)) {
        return luaL_error(state, "activity squad is stale");
    }
    refuse_unknown_arguments(state, kActorCommand);
    const lua_Integer command = checked_integer_argument(
        state, required_key(kActorCommand, "command", api::ValueKind::integer));
    const lua_Integer value = checked_integer_argument(
        state, required_key(kActorCommand, "value", api::ValueKind::integer));
    if (command < 0
        || static_cast<std::uint64_t>(command) > (std::numeric_limits<std::uint32_t>::max)()
        || value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "actor command scalar is outside its durable type");
    }
    Impl* const impl = impl_from_state(state);
    Intent intent{};
    if (impl == nullptr
        || !decode_sdk_build_sha256(std::string_view(impl->identity.sdkBuildId.data()),
                                    intent.sdkBuildSha256)) {
        return luaL_error(state, "loaded SDK generation identity is invalid");
    }
    intent.kind = IntentKind::actorCommand;
    intent.firstRow = definition.nativeRow;
    intent.actorCommandSelector = static_cast<std::uint32_t>(command);
    intent.actorCommandValue = static_cast<std::int32_t>(value);
    return queue_intent(state, active_frame(state), intent);
}

static_assert(state::activity::mission::kSquadTemperamentCapacity
              == middleware::bap::activity_message::scriptable_auth::kType2TemperamentCapacity);

bool read_temperament_list(
    lua_State* state,
    const char* key,
    std::uint32_t slotRow,
    std::array<std::uint32_t,
               middleware::bap::activity_message::scriptable_auth::kType2TemperamentCapacity>&
        output,
    std::uint8_t& count) {
    constexpr const char* kOwnerKey =
        api::find_field(api::kActorTemperamentRowFields, "slot_row").name.data();
    constexpr const char* kIdKey =
        api::find_field(api::kActorTemperamentRowFields, "id").name.data();
    const Impl* const impl = impl_from_state(state);
    output = {};
    count = 0;
    lua_getfield(state, 2, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const std::size_t listed = lua_rawlen(state, -1);
    if (listed > output.size()) {
        static_cast<void>(luaL_error(
            state, "%s names more than %d temperaments", key, static_cast<int>(output.size())));
    }
    for (std::size_t index = 0; index < listed; ++index) {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        luaL_checktype(state, -1, LUA_TTABLE);
        const int row = lua_gettop(state);
        const lua_Integer owner = directive_integer(state, row, kOwnerKey);
        const lua_Integer value = directive_integer(state, row, kIdKey);
        if (owner != static_cast<lua_Integer>(slotRow) || value <= 0
            || value > (std::numeric_limits<std::uint32_t>::max)()
            || impl->definitions.resolveActorTemperament == nullptr
            || !impl->definitions.resolveActorTemperament(
                impl->definitions.context, slotRow, static_cast<std::uint32_t>(value))) {
            static_cast<void>(
                luaL_error(state, "%s must name mission.ActorTemperament rows of this slot", key));
        }
        const auto id = static_cast<std::uint32_t>(value);
        const auto before = output.begin() + static_cast<std::ptrdiff_t>(index);
        if (std::find(output.begin(), before, id) != before) {
            static_cast<void>(luaL_error(state, "%s must be distinct", key));
        }
        output[index] = id;
        lua_pop(state, 1);
    }
    count = static_cast<std::uint8_t>(listed);
    lua_pop(state, 1);
    return true;
}

/** Reads a squad's temperament list into the placement or assignment it stages. */
bool read_squad_temperaments(lua_State* state,
                             const char* key,
                             std::uint32_t slotRow,
                             Intent& intent) {
    std::array<std::uint32_t,
               middleware::bap::activity_message::scriptable_auth::kType2TemperamentCapacity>
        ids{};
    std::uint8_t count = 0;
    if (!read_temperament_list(state, key, slotRow, ids, count)) {
        return false;
    }
    std::copy(ids.begin(), ids.end(), intent.squadTemperaments.begin());
    intent.squadTemperamentCount = count;
    return true;
}

/**
 * Stages one squad placement: squad:place{counts, mode, retire_on_return, spawn_rule,
 * temperaments}. Every parameter is optional; the counts and mode carry their own bounds. A spawn
 * rule is a type-66 slot that replaces the package rule.
 */
[[nodiscard]] int squad_place(lua_State* state) {
    const auto* const handle =
        static_cast<const SquadHandle*>(luaL_checkudata(state, 1, kSquadMetatable));
    SquadDefinition definition{};
    if (!current_squad(state, *handle, definition) || definition.memberCount == 0
        || definition.memberCount > kSquadMemberCapacity) {
        return luaL_error(state, "activity squad is stale or invalid");
    }
    refuse_unknown_arguments(state, kPlace);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::placeSquad;
    intent.firstRow = definition.nativeRow;
    intent.squadCount = static_cast<std::uint8_t>(definition.memberCount);

    SquadCountVectorHandle counts{};
    if (optional_argument(state,
                          optional_key(kPlace, "counts", api::ValueKind::handle),
                          kSquadCountVectorMetatable,
                          counts)) {
        // A count vector is bound to one squad, and no type carries that binding.
        if (counts.squadRow != handle->localRow) {
            return luaL_error(state, "squad counts were minted by another squad");
        }
        std::copy_n(counts.counts.begin(), counts.count, intent.squadCounts.begin());
    } else {
        std::copy_n(
            definition.defaultCounts.begin(), definition.memberCount, intent.squadCounts.begin());
    }

    SquadModeHandle mode{};
    static_cast<void>(optional_argument(
        state, optional_key(kPlace, "mode", api::ValueKind::enumValue), kSquadModeMetatable, mode));
    intent.squadMode = mode.mode;
    lua_getfield(state, 2, optional_key(kPlace, "spawn_rule", api::ValueKind::handle));
    if (!lua_isnil(state, -1)) {
        const auto* const rule =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition ruleSlot{};
        if (!current_slot(state, *rule, ruleSlot)
            || ruleSlot.slotType
                   != middleware::content::packages::tables::kAuthoredSquadRuleSlotType) {
            return luaL_error(state, "spawn_rule requires a current authored type-66 slot");
        }
        intent.squadSpawnRuleRow = static_cast<std::int32_t>(ruleSlot.nativeRow);
    }
    lua_pop(state, 1);
    intent.squadRetireOnReturn =
        optional_boolean_argument(state, boolean_option(kPlace, "retire_on_return"));
    intent.squadTemperamentsSet =
        read_squad_temperaments(state,
                                optional_key(kPlace, "temperaments", api::ValueKind::list),
                                definition.slotRow,
                                intent);
    return queue_intent(state, frame, intent);
}

void register_squad_metatables(lua_State* state) {
    register_metatable(state, kSquadMetatable, &squad_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
