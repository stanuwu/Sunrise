#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <tuple>

#include "../../../core/runtime/server_clock.h"
#include "../../../middleware/bap/activity_message/incident.h"
#include "../../../middleware/crypto/random_bytes.h"
#include "../../../state/activity/membership/definition.h"
#include "../../../state/build_data/public_event_schedule/definition.h"
#include "../../../state/mods/definition.h"
#include "../activity_clock.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_peer_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"
#include "public_event_schedule_evaluate.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;

// Table entries of the context members this file implements.
constexpr const api::MemberSpec& kActivateObjects =
    api::member(api::Receiver::context, "activate_objects");
constexpr const api::MemberSpec& kCompleteMission =
    api::member(api::Receiver::context, "complete_mission");
constexpr const api::MemberSpec& kRestartCheckpoint =
    api::member(api::Receiver::context, "restart_checkpoint");
constexpr const api::MemberSpec& kHoldSpawn = api::member(api::Receiver::context, "hold_spawn");
constexpr const api::MemberSpec& kPublicEventSchedule =
    api::member(api::Receiver::context, "public_event_schedule");

constexpr const api::MemberSpec& kSendIncident =
    api::member(api::Receiver::context, "send_incident");
constexpr const api::MemberSpec& kActivityFlag =
    api::member(api::Receiver::context, "set_activity_flag");
constexpr const api::MemberSpec& kSelectState = api::member(api::Receiver::context, "select_state");

/** Draws a server-owned random fraction for one active callback. */
[[nodiscard]] int context_random(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    static_cast<void>(active_frame(state));
    std::uint64_t seed = 0;
    if (!::sunrise::middleware::crypto::random::draw_seed(seed)) {
        return luaL_error(state, "server randomness is unavailable");
    }
    constexpr int kNumberBits = std::numeric_limits<lua_Number>::digits;
    constexpr int kSeedBits = std::numeric_limits<std::uint64_t>::digits;
    static_assert(std::numeric_limits<lua_Number>::radix == 2 && kNumberBits < kSeedBits);
    // Keep only exactly representable bits so the largest draw remains below one.
    constexpr auto kFractionCount = std::uint64_t{1} << kNumberBits;
    lua_pushnumber(state,
                   static_cast<lua_Number>(seed >> (kSeedBits - kNumberBits))
                       / static_cast<lua_Number>(kFractionCount));
    return 1;
}

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
    const bool capturePlate =
        optional_boolean_argument(state, boolean_option(kActivateObjects, "capture_plate"));
    // The position lane is the object's own state input; it is sent at the current generation.
    bool positionChild = false;
    lua_Number positionValue = 0;
    bool positionSnap = true;
    if (push_argument(state, optional_key(kActivateObjects, "position", api::ValueKind::table))
        != LUA_TNIL) {
        luaL_checktype(state, -1, LUA_TTABLE);
        refuse_unknown_fields(state, -1, api::find_argument(kActivateObjects, "position").fields);
        lua_getfield(
            state,
            -1,
            table_field_key(kActivateObjects, "position", "value", api::ValueKind::number));
        const bool numeric = lua_isnumber(state, -1) != 0;
        positionValue = numeric ? lua_tonumber(state, -1) : -1;
        lua_pop(state, 1);
        lua_getfield(
            state,
            -1,
            table_field_key(kActivateObjects, "position", "snap", api::ValueKind::boolean, true));
        if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
            return luaL_error(state, "position.snap must be a boolean");
        }
        positionSnap = lua_isnil(state, -1) || lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        if (!numeric || !(positionValue >= 0 && positionValue <= 1) || capturePlate || !active) {
            return luaL_error(state,
                              "position needs 0 <= value <= 1, on an active activation without "
                              "capture_plate");
        }
        positionChild = true;
    }
    lua_pop(state, 1);
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
        intent.capturePlate = capturePlate;
        intent.positionChild = positionChild;
        intent.positionValue = static_cast<float>(positionValue);
        intent.positionSnap = positionSnap;
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

/** Sets one integer field of the result table on top of the stack. */
void set_integer_field(lua_State* state, const char* name, lua_Integer value) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, name);
}

/**
 * Evaluates the Director's public-event schedule for one site at the host clock.
 * @param state Lua call holding the context and a table with `site` and an optional `now`.
 * @return One result table, or nil when the table is unavailable or lists no such site.
 */
[[nodiscard]] int context_public_event_schedule(lua_State* state) {
    namespace store = ::sunrise::state::build_data::public_event_schedule;
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kPublicEventSchedule);
    const lua_Integer site = checked_integer_argument(
        state, required_key(kPublicEventSchedule, "site", api::ValueKind::integer));
    const lua_Integer requested =
        optional_integer_argument(state, integer_option(kPublicEventSchedule, "now"));
    const std::int64_t now = requested != 0 ? requested : core::runtime::server_clock_seconds();
    if (site <= 0 || static_cast<std::uint64_t>(site) > UINT32_MAX) {
        return luaL_error(state, "schedule site must be a positive uint32");
    }
    public_event_schedule::SiteResult result{};
    const auto table = store::snapshot();
    if (!public_event_schedule::for_site(*table, static_cast<std::uint32_t>(site), now, result)) {
        lua_pushnil(state);
        return 1;
    }
    constexpr auto fields = std::span(api::kPublicEventScheduleFields);
    const auto& at = result.occurrence;
    lua_createtable(state, 0, static_cast<int>(fields.size()));
    lua_pushstring(state, at.active ? "active" : "incoming");
    lua_setfield(state, -2, api::find_field(fields, "state").name.data());
    set_integer_field(state, api::find_field(fields, "until_start").name.data(), at.untilStart);
    set_integer_field(state, api::find_field(fields, "until_finish").name.data(), at.finish - now);
    set_integer_field(state, api::find_field(fields, "slot_start").name.data(), at.slotStart);
    set_integer_field(state, api::find_field(fields, "start").name.data(), at.start);
    set_integer_field(state, api::find_field(fields, "finish").name.data(), at.finish);
    set_integer_field(
        state, api::find_field(fields, "slot").name.data(), static_cast<lua_Integer>(at.slot));
    set_integer_field(state,
                      api::find_field(fields, "row").name.data(),
                      static_cast<lua_Integer>(result.row) + 1);
    lua_pushboolean(state, result.selected ? 1 : 0);
    lua_setfield(state, -2, api::find_field(fields, "selected").name.data());
    if (result.picked != public_event_schedule::kNoEntry) {
        const auto& entry = table->entries[result.picked];
        set_integer_field(state, api::find_field(fields, "site").name.data(), entry.site);
        if (entry.node != store::kNoNode) {
            set_integer_field(state, api::find_field(fields, "node").name.data(), entry.node);
        }
        if (entry.rally != store::kAbsentHash) {
            set_integer_field(state, api::find_field(fields, "rally").name.data(), entry.rally);
        }
    }
    return 1;
}

/**
 * Raises or lowers one activity flag, the id list every participation record carries.
 * @param state Lua call holding the context, a `key` and `raised`.
 * @return One RequestKey.
 */
[[nodiscard]] int context_set_activity_flag(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    refuse_unknown_arguments(state, kActivityFlag);
    const lua_Integer key = checked_integer_argument(
        state, required_key(kActivityFlag, "key", api::ValueKind::integer));
    if (key <= 0 || key > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "activity flag key is zero or out of range");
    }
    Intent intent{};
    intent.kind = IntentKind::setActivityFlag;
    intent.activityFlagKey = static_cast<std::uint32_t>(key);
    lua_getfield(state, 2, required_key(kActivityFlag, "raised", api::ValueKind::boolean));
    if (!lua_isboolean(state, -1)) {
        return luaL_argerror(state, 2, "set_activity_flag requires a raised boolean");
    }
    intent.active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return queue_intent(state, active_frame(state), intent);
}

/**
 * Sends one activity incident (message 19) to the client. The client indexes its handler tables
 * with the target unchecked, so out-of-range and poisoned targets are refused here.
 * @param state Lua call holding the context and `{target, payload?}`. An absent payload sends the
 * all-zero notification body; `""` sends none.
 * @return One RequestKey. Its effect result is `queued`; host delivery events report the stage.
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
        intent.authBody.assign(incident::kNotificationPayloadBytes, std::byte{0});
        intent.authByteCount = static_cast<std::uint16_t>(incident::kNotificationPayloadBytes);
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
        lua_getfield(state,
                     3,
                     table_field_key(
                         kSelectState, "options", "spawn_set_hash", api::ValueKind::integer, true));
        if (!lua_isnil(state, -1)) {
            const lua_Integer hash = lua_isinteger(state, -1) ? lua_tointeger(state, -1) : 0;
            if (hash <= 0 || hash >= (std::numeric_limits<std::uint32_t>::max)()) {
                return luaL_argerror(state, 3, "spawn_set_hash must be an authored spawn-set hash");
            }
            intent.arrivalSpawnHash = static_cast<std::uint32_t>(hash);
        }
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
    // Scripts align public-event schedules with the client, which reads the same wall clock.
    {"host_unix_seconds",
     nullptr,
     [](lua_State* state, const Impl&) {
         lua_pushinteger(state, core::runtime::server_clock_seconds());
     }},
    {"public_event_schedule", &context_public_event_schedule},
    {"complete_mission", &context_complete_mission},
    {"activate_objects", &context_activate_objects},
    {"cohort", &context_cohort},
    {"peers", nullptr, [](lua_State* state, const Impl&) { push_peers(state); }},
    {"squad", &context_squad},
    {"scene", &context_scene},
    {"slot", &context_slot},
    {"hold_spawn", &context_hold_spawn},
    {"send_incident", &context_send_incident},
    {"set_activity_flag", &context_set_activity_flag},
    {"select_state", &context_select_state},
    {"restart_checkpoint", &context_restart_checkpoint},
    {"random", &context_random},
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
