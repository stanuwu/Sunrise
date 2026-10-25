#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <tuple>

#include "../../../middleware/bap/activity_message/incident.h"
#include "../../../state/activity/membership/definition.h"
#include "../../../state/mods/definition.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_peer_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Table entries of the context members this file implements.
constexpr const api::MemberSpec& kActivateObjects =
    api::member(api::Receiver::context, "activate_objects");
constexpr const api::MemberSpec& kCompleteMission =
    api::member(api::Receiver::context, "complete_mission");
constexpr const api::MemberSpec& kRestartCheckpoint =
    api::member(api::Receiver::context, "restart_checkpoint");
constexpr const api::MemberSpec& kHoldSpawn = api::member(api::Receiver::context, "hold_spawn");
constexpr const api::MemberSpec& kSendIncident =
    api::member(api::Receiver::context, "send_incident");
constexpr const api::MemberSpec& kSelectState = api::member(api::Receiver::context, "select_state");

/**
 * Groups exact authored objects by native owner and reserves bounded output batches.
 * @param state Lua call holding the context and named object list.
 * @return One array of RequestKeys.
 */
[[nodiscard]] int context_activate_objects(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kActivateObjects);
    const bool active =
        optional_boolean_argument(state, boolean_option(kActivateObjects, "active"));
    lua_getfield(state, 2, required_key(kActivateObjects, "slots", api::ValueKind::list));
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    std::size_t entries = 0;
    lua_pushnil(state);
    while (lua_next(state, list) != 0) {
        if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1
            || static_cast<std::uint64_t>(lua_tointeger(state, -2)) > count) {
            return luaL_error(state, "object collection must be a dense list");
        }
        ++entries;
        lua_pop(state, 1);
    }
    if (entries != count) {
        return luaL_error(state, "object collection must be a dense list");
    }
    CallFrame& frame = active_frame(state);
    frame.candidate.objectBatch.clear();
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const auto* handle =
            static_cast<const SlotHandle*>(luaL_testudata(state, -1, kSlotMetatable));
        SlotDefinition slot{};
        namespace format = ::sunrise::state::activity_sdk::format;
        const bool resolved =
            handle != nullptr ? current_slot(state, *handle, slot) : resolve_slot(state, -1, slot);
        if (!resolved || slot.slotType != format::kObjectSlotType
            || slot.componentClass != format::kObjectComponentClass
            || slot.senseSchema != format::kObjectSenseSchema
            || slot.authSchema != format::kObjectAuthSchema
            || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
            return luaL_error(state, "object collection requires exact authored type-4 slots");
        }
        lua_pop(state, 1);
        try {
            frame.candidate.objectBatch.push_back(slot);
        } catch (const std::bad_alloc&) {
            frame.intentAllocationFailed = true;
            return luaL_error(state, "object collection allocation failed");
        }
    }
    auto& slots = frame.candidate.objectBatch;
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) noexcept {
        return std::tie(a.objectTag, a.registryKey, a.nativeRow)
               < std::tie(b.objectTag, b.registryKey, b.nativeRow);
    });
    for (std::size_t index = 1; index < slots.size(); ++index) {
        if (slots[index - 1].nativeRow == slots[index].nativeRow) {
            return luaL_error(state, "object collection repeats a SlotView");
        }
    }
    lua_newtable(state);
    lua_Integer requestCount = 0;
    for (std::size_t first = 0; first < slots.size();) {
        Intent intent{};
        intent.kind = IntentKind::setObjectActive;
        intent.active = active;
        intent.firstRow = slots[first].nativeRow;
        std::size_t next = first + 1;
        while (next < slots.size() && slots[next].objectTag == slots[first].objectTag
               && slots[next].registryKey == slots[first].registryKey
               && intent.burstRowCount < intent.burstRows.size()) {
            intent.burstRows[intent.burstRowCount++] = slots[next++].nativeRow;
        }
        static_cast<void>(queue_intent(state, frame, intent));
        lua_rawseti(state, -2, ++requestCount);
        first = next;
    }
    return 1;
}

/**
 * Completes this attempt through the native lifetime owner.
 * @param state Lua call holding the context and empty argument table.
 * @return One RequestKey.
 */
[[nodiscard]] int context_complete_mission(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kCompleteMission);
    if (impl_from_state(state)->attempt.complete) {
        return luaL_error(state, "mission attempt is already complete");
    }
    Intent intent{};
    intent.kind = IntentKind::setLifetime;
    intent.lifetimeState = ::sunrise::state::activity::mission::kCompletedLifetimeState;
    return queue_intent(state, active_frame(state), intent);
}

[[nodiscard]] int context_squad(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SquadDefinition definition{};
    if (!resolve_squad(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity squad");
    }
    push_handle(state, kSquadMetatable, SquadHandle{definition.localRow});
    return 1;
}

/**
 * Arms a native hard wipe at an authored spawn set, or releases one with its request key.
 * The Lua caller passes `release_request` as the decimal string of the original key.
 */
[[nodiscard]] int context_restart_checkpoint(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kRestartCheckpoint);
    const lua_Integer region = checked_integer_argument(
        state, required_key(kRestartCheckpoint, "region", api::ValueKind::integer));
    const lua_Integer hash = checked_integer_argument(
        state, required_key(kRestartCheckpoint, "spawn_set_hash", api::ValueKind::integer));
    if (region < 0 || region > ::sunrise::state::activity::membership::kMaximumSliceSetIndex
        || hash <= 0 || hash >= (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "checkpoint requires an authored region and spawn-set hash");
    }
    std::uint64_t release = 0;
    lua_getfield(
        state,
        2,
        optional_key(kRestartCheckpoint, "release_request", api::ValueKind::decimalString));
    if (!lua_isnil(state, -1)) {
        std::size_t length = 0;
        const char* const value = luaL_checklstring(state, -1, &length);
        const auto parsed = std::from_chars(value, value + length, release);
        if (parsed.ec != std::errc{} || parsed.ptr != value + length || release == 0) {
            return luaL_error(state, "checkpoint release requires the original RequestKey.value");
        }
    }
    lua_pop(state, 1);
    Intent intent{};
    intent.kind = IntentKind::restartCheckpoint;
    intent.checkpointReleaseRequest = release;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    intent.checkpointSpawnHash = static_cast<std::uint32_t>(hash);
    return queue_intent(state, active_frame(state), intent);
}

/**
 * Holds the player's spawn, or releases it.
 * A held spawn leaves the player without a body, so an opening cutscene runs before the arrival.
 * @param state Lua call holding the context and one `active` boolean.
 * @return One RequestKey.
 */
[[nodiscard]] int context_hold_spawn(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kHoldSpawn);
    lua_getfield(state, 2, required_key(kHoldSpawn, "active", api::ValueKind::boolean));
    if (!lua_isboolean(state, -1)) {
        return luaL_argerror(state, 2, "hold_spawn requires an active boolean");
    }
    Intent intent{};
    intent.kind = IntentKind::holdSpawn;
    intent.active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return queue_intent(state, active_frame(state), intent);
}

/**
 * Sends one activity incident (message 19) to the client, as the activity host would.
 * The target is an sobject row, the 13-bit incident target the client resolves to a handler. The
 * Last Wish HUD callouts are type-0 rows: 5124 "Kalli prepares to wield her weapon", 5330 "Kalli
 * conceives an ontological weapon". Out-of-range and poisoned targets are refused here, because
 * the client indexes its handler tables with them unchecked.
 * @param state Lua call holding the context and `{target, payload?}`; `payload` is a raw byte
 * string of at most 500 bytes. Absent sends the all-zero 42-byte metadata header, which is what a
 * type-0 notification row needs; pass `""` to send no payload at all.
 * @return One RequestKey. Its effect result reports that the host queued the incident.
 */
[[nodiscard]] int context_send_incident(lua_State* state) {
    namespace incident = ::sunrise::middleware::bap::activity_message::incident;
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kSendIncident);
    lua_getfield(state, 2, required_key(kSendIncident, "target", api::ValueKind::integer));
    const lua_Integer target = lua_isinteger(state, -1) ? lua_tointeger(state, -1) : -1;
    lua_pop(state, 1);
    if (target < 0 || target > static_cast<lua_Integer>(incident::kTargetMaximum)) {
        return luaL_error(state,
                          "send_incident requires a target from 0 to %d",
                          static_cast<int>(incident::kTargetMaximum));
    }
    Intent intent{};
    intent.kind = IntentKind::sendIncident;
    intent.firstRow = static_cast<std::uint32_t>(target);
    lua_getfield(state, 2, optional_key(kSendIncident, "payload", api::ValueKind::string));
    if (lua_isnil(state, -1)) {
        intent.authBody.assign(incident::kHeaderOnlyPayloadBytes, std::byte{0});
        intent.authByteCount = static_cast<std::uint16_t>(incident::kHeaderOnlyPayloadBytes);
    } else {
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_argerror(state, 2, "send_incident payload must be a byte string");
        }
        std::size_t length = 0;
        const char* const bytes = lua_tolstring(state, -1, &length);
        if (length > incident::kPayloadMaximum) {
            return luaL_error(state,
                              "send_incident payload exceeds %d bytes",
                              static_cast<int>(incident::kPayloadMaximum));
        }
        const auto* const first = reinterpret_cast<const std::byte*>(bytes);
        intent.authBody.assign(first, first + length);
        intent.authByteCount = static_cast<std::uint16_t>(length);
    }
    lua_pop(state, 1);
    incident::Incident probe{};
    probe.primaryTarget = intent.firstRow;
    probe.payloadLength = intent.authByteCount;
    if (!incident::outer_valid(probe)) {
        return luaL_error(state, "send_incident target %d is poisoned", static_cast<int>(target));
    }
    return queue_intent(state, active_frame(state), intent);
}

[[nodiscard]] int context_scene(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SceneDefinition definition{};
    if (!resolve_scene(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous authored scene");
    }
    push_handle(state, kSceneMetatable, SceneHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int context_slot(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SlotDefinition definition{};
    if (!resolve_slot(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity slot");
    }
    push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    return 1;
}

} // namespace

/** Resolves the squad one Lua argument names, by one-based row or by id. */
[[nodiscard]] bool resolve_squad(lua_State* state, int selector, SquadDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSquadRow != nullptr
               && impl->definitions.resolveSquadRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSquadId != nullptr
           && impl->definitions.resolveSquadId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the scene one Lua argument names, by one-based row or by id. */
[[nodiscard]] bool resolve_scene(lua_State* state, int selector, SceneDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSceneRow != nullptr
               && impl->definitions.resolveSceneRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSceneId != nullptr
           && impl->definitions.resolveSceneId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the slot one Lua argument names, by one-based row or by id. */
[[nodiscard]] bool resolve_slot(lua_State* state, int selector, SlotDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSlotRow != nullptr
               && impl->definitions.resolveSlotRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSlotId != nullptr
           && impl->definitions.resolveSlotId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

[[nodiscard]] bool
resolve_message_name(lua_State* state, std::string_view name, ActivityMessageDefinition& output) {
    Impl* const impl = impl_from_state(state);
    return impl != nullptr && impl->definitions.resolveActivityMessageName != nullptr
           && impl->definitions.resolveActivityMessageName(impl->definitions.context, name, output);
}

/**
 * Reads the optional omit list: generated slots whose owning object stays out of the seed.
 * @return False with the Lua error already raised.
 */
[[nodiscard]] bool parse_seed_omissions(lua_State* state, int index, Intent& intent) {
    if (lua_isnoneornil(state, index)) {
        return true;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, index));
    if (count < 0
        || static_cast<std::size_t>(count)
               > ::sunrise::state::activity::mission::kMissionSeedOmitCapacity) {
        static_cast<void>(luaL_argerror(state, index, "mission seed omit list is too long"));
        return false;
    }
    for (lua_Integer entry = 1; entry <= count; ++entry) {
        lua_rawgeti(state, index, entry);
        SlotDefinition definition{};
        const bool resolved = resolve_slot(state, lua_gettop(state), definition);
        lua_pop(state, 1);
        if (!resolved) {
            static_cast<void>(luaL_argerror(state, index, "unknown or ambiguous activity slot"));
            return false;
        }
        intent.seedOmissions[static_cast<std::size_t>(entry - 1)] = {definition.objectTag,
                                                                     definition.registryKey};
    }
    intent.seedOmissionCount = static_cast<std::uint8_t>(count);
    return true;
}

/** Queues one generated mission state by its authored effective region. */
[[nodiscard]] int context_select_state(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(
        state, 2, table_field_key(kSelectState, "state", "region_index", api::ValueKind::integer));
    if (!lua_isinteger(state, -1)) {
        return luaL_argerror(state, 2, "generated mission state has no integer region_index");
    }
    const lua_Integer region = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (region < 0
        || static_cast<std::uint64_t>(region)
               > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return luaL_argerror(state, 2, "generated mission state region_index is outside i32");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::selectMissionState;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    if (!parse_seed_omissions(state, 3, intent)) {
        return 0;
    }
    if (lua_istable(state, 3)) {
        refuse_unknown_fields(state, 3, api::find_argument(kSelectState, "options").fields);
        lua_getfield(
            state,
            3,
            table_field_key(
                kSelectState, "options", "retire_placed_props", api::ValueKind::boolean, true));
        if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
            return luaL_argerror(state, 3, "retire_placed_props must be a boolean");
        }
        intent.retirePlacedProps = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }
    return queue_intent(state, frame, intent);
}

namespace {

namespace mods = ::sunrise::state::mods;

/** Reads one declared value of the program's mod at each access, so a change applies at once. */
[[nodiscard]] int mod_config_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kModConfigMetatable));
    const Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    mods::ConfigValue value{};
    if (impl->identity.readModConfig == nullptr
        || !impl->identity.readModConfig(impl->identity.modId.data(), key, value)) {
        lua_pushnil(state);
        return 1;
    }
    switch (value.type) {
    case mods::ConfigType::integer:
        lua_pushinteger(state, value.integer);
        break;
    case mods::ConfigType::floating: {
        // The shortest text of the float round-trips, so 1.1 reaches Lua as 1.1, not float noise.
        std::array<char, 32> text{};
        double number = value.floating;
        const auto written = std::to_chars(text.data(), text.data() + text.size(), value.floating);
        if (written.ec == std::errc{}) {
            static_cast<void>(std::from_chars(text.data(), written.ptr, number));
        }
        lua_pushnumber(state, number);
        break;
    }
    case mods::ConfigType::boolean:
        lua_pushboolean(state, value.boolean ? 1 : 0);
        break;
    }
    return 1;
}

using ContextBinding = Binding<Impl>;

/** Context members in table order. */
constexpr std::array<ContextBinding, std::size(api::kContextMembers)> kContextBindings{{
    {"sdk_build_id",
     nullptr,
     [](lua_State* state, const Impl& impl) {
         lua_pushstring(state, impl.identity.sdkBuildId.data());
     }},
    {"activity_id",
     nullptr,
     [](lua_State* state, const Impl& impl) {
         lua_pushstring(state, impl.identity.activityId.data());
     }},
    {"activity_row",
     nullptr,
     [](lua_State* state, const Impl& impl) { lua_pushinteger(state, impl.identity.activityRow); }},
    {"definition_hash",
     nullptr,
     [](lua_State* state, const Impl& impl) {
         lua_pushinteger(state, impl.identity.definitionHash);
     }},
    {"activity_role",
     nullptr,
     [](lua_State* state, const Impl& impl) {
         lua_pushstring(state, impl.identity.publicTarget ? "public" : "private");
     }},
    {"player_key",
     nullptr,
     [](lua_State* state, const Impl& impl) { push_u64_string(state, impl.identity.playerKey); }},
    {"sdk", nullptr, [](lua_State* state, const Impl&) { push_activity(state); }},
    {"lifetime", nullptr, [](lua_State* state, const Impl&) { push_lifetime(state); }},
    {"attempt_generation",
     nullptr,
     [](lua_State* state, const Impl& impl) { push_u64_string(state, impl.attempt.generation); }},
    {"mission_complete",
     nullptr,
     [](lua_State* state, const Impl& impl) { lua_pushboolean(state, impl.attempt.complete); }},
    {"complete_mission", &context_complete_mission},
    {"activate_objects", &context_activate_objects},
    {"cohort", &context_cohort},
    {"peers", nullptr, [](lua_State* state, const Impl&) { push_peers(state); }},
    {"squad", &context_squad},
    {"scene", &context_scene},
    {"slot", &context_slot},
    {"hold_spawn", &context_hold_spawn},
    {"send_incident", &context_send_incident},
    {"select_state", &context_select_state},
    {"restart_checkpoint", &context_restart_checkpoint},
    {"set_phase", &context_set_phase},
    {"set_variable", &context_set_variable},
    {"clear_variable", &context_clear_variable},
    {"start_timer", &context_start_timer},
    {"cancel_timer", &context_cancel_timer},
    // A script no mod ships gets the handle too; every key it reads is nil.
    {"mod_config", nullptr, [](lua_State* state, const Impl&) { push_mod_config(state); }},
    {"timers", nullptr, [](lua_State* state, const Impl&) { push_timer_ref_collection(state); }},
    {"variables",
     nullptr,
     [](lua_State* state, const Impl&) { push_variable_ref_collection(state); }},
}};
static_assert(bound_to<Impl>(api::kContextMembers, kContextBindings));

} // namespace

/** Lua index for the mission context: its collections, phase, variables and timers. */
[[nodiscard]] int context_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const Impl* const impl = impl_from_state(state);
    return dispatch<Impl>(state, kContextBindings, lua_string_view(state, 2), *impl);
}

void register_context_metatables(lua_State* state) {
    register_metatable(state, kContextMetatable, &context_index);
    register_metatable(state, kModConfigMetatable, &mod_config_index);
    register_population_metatables(state);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
