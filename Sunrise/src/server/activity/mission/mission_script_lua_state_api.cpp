#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../../core/logging/log.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/**
 * Finds one named row in the callback candidate.
 * @param found Set when the name matched; the row index is meaningless otherwise.
 * @return The matching row index, or the count when absent.
 */
template <typename Rows>
[[nodiscard]] std::size_t
find_state_row(const Rows& rows, std::size_t count, const StateKey& key, bool& found) noexcept {
    const std::string_view selected = state_key_view(key);
    std::size_t first = 0;
    std::size_t last = count;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        if (state_key_view(rows[middle].key) < selected) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    found = first < count && state_key_view(rows[first].key) == selected;
    return first;
}

/** Reads one boolean, signed integer, finite real, or bounded string. */
[[nodiscard]] bool
parse_variable_value(lua_State* state, int index, VariableValue& output) noexcept {
    output = {};
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN:
        output.kind = VariableValueKind::boolean;
        output.booleanValue = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) {
            output.kind = VariableValueKind::integer;
            output.integerValue = static_cast<std::int64_t>(lua_tointeger(state, index));
            return true;
        }
        output.kind = VariableValueKind::real;
        output.realValue = static_cast<double>(lua_tonumber(state, index));
        return std::isfinite(output.realValue);
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* const value = lua_tolstring(state, index, &length);
        if (value == nullptr || length >= output.stringValue.size()) {
            return false;
        }
        output.kind = VariableValueKind::string;
        std::copy_n(value, length, output.stringValue.data());
        output.stringLength = static_cast<std::uint16_t>(length);
        return true;
    }
    default:
        return false;
    }
}

/** Pushes one stored variable in the type it was written as. */
void push_variable_value(lua_State* state, const VariableValue& value) {
    switch (value.kind) {
    case VariableValueKind::boolean:
        lua_pushboolean(state, value.booleanValue ? 1 : 0);
        return;
    case VariableValueKind::integer:
        lua_pushinteger(state, static_cast<lua_Integer>(value.integerValue));
        return;
    case VariableValueKind::real:
        lua_pushnumber(state, static_cast<lua_Number>(value.realValue));
        return;
    case VariableValueKind::string:
        lua_pushlstring(state, value.stringValue.data(), value.stringLength);
        return;
    }
    lua_pushnil(state);
}

[[nodiscard]] std::uint64_t timer_deadline(std::uint64_t now, std::uint32_t delay) noexcept {
    return now > (std::numeric_limits<std::uint64_t>::max)() - delay
               ? (std::numeric_limits<std::uint64_t>::max)()
               : now + delay;
}

/** Reads one candidate variable, or nil when it is absent. */
[[nodiscard]] int state_variable(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kStateMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    const CallFrame& frame = active_frame(state);
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.variables, frame.candidate.variableCount, key, found);
    if (!found) {
        lua_pushnil(state);
    } else {
        push_variable_value(state, frame.candidate.variables[row].value);
    }
    return 1;
}

/** Tests the current callback candidate for one variable. */
[[nodiscard]] int state_has_variable(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kStateMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    const CallFrame& frame = active_frame(state);
    bool found = false;
    static_cast<void>(
        find_state_row(frame.candidate.variables, frame.candidate.variableCount, key, found));
    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}

/** Reads one candidate timer deadline as a decimal string, or nil when absent. */
[[nodiscard]] int state_timer(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kStateMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid");
    }
    const CallFrame& frame = active_frame(state);
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.timers, frame.candidate.timerCount, key, found);
    if (!found) {
        lua_pushnil(state);
    } else {
        push_u64_string(state, frame.candidate.timers[row].deadlineTick);
    }
    return 1;
}

/** State view members in table order. */
constexpr std::array<Binding<Impl>, std::size(api::kStateMembers)> kStateBindings{{
    {"phase",
     nullptr,
     [](lua_State* state, const Impl& impl) {
         const CallFrame& frame = active_frame(state);
         lua_pushinteger(state, frame.candidate.phaseChanged ? frame.candidate.phase : impl.phase);
     }},
    {"revision",
     nullptr,
     [](lua_State* state, const Impl& impl) { push_u64_string(state, impl.stateRevision); }},
    {"variable", &state_variable},
    {"has_variable", &state_has_variable},
    {"timer", &state_timer},
}};
static_assert(bound_to<Impl>(api::kStateMembers, kStateBindings));

} // namespace

/** Stages a phase change that commits with the enclosing callback. */
[[nodiscard]] int context_set_phase(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const lua_Integer phase = luaL_checkinteger(state, 2);
    if (phase < 0
        || static_cast<std::uint64_t>(phase) > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "mission phase is outside u32");
    }
    CallFrame& frame = active_frame(state);
    frame.candidate.phase = static_cast<std::uint32_t>(phase);
    frame.candidate.phaseChanged = true;
    return 0;
}
/** Logs one staged variable write, so the gate state a controller keeps in variables is visible. */
void log_variable_write(const StateKey& key, const VariableValue& value) noexcept {
    std::array<char, 32> scalar{};
    std::string_view text{};
    int scalarWritten = 0;
    switch (value.kind) {
    case VariableValueKind::boolean:
        text = value.booleanValue ? "true" : "false";
        break;
    case VariableValueKind::integer:
        scalarWritten = std::snprintf(
            scalar.data(), scalar.size(), "%lld", static_cast<long long>(value.integerValue));
        break;
    case VariableValueKind::real:
        scalarWritten = std::snprintf(scalar.data(), scalar.size(), "%.6g", value.realValue);
        break;
    case VariableValueKind::string:
        text = {value.stringValue.data(),
                (std::min)(std::size_t{value.stringLength}, value.stringValue.size())};
        break;
    }
    if (scalarWritten > 0) {
        text = {scalar.data(),
                (std::min)(static_cast<std::size_t>(scalarWritten), scalar.size() - 1)};
    }
    const std::string_view name = state_key_view(key);
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=mission_script stage=variable result=set name=%.*s value=%.*s",
                      static_cast<int>(name.size()),
                      name.data(),
                      static_cast<int>(text.size()),
                      text.data());
    if (written > 0) {
        core::log::write(
            core::log::Channel::server,
            core::log::Level::debug,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/** Stages one variable write that commits with the enclosing callback. */
[[nodiscard]] int context_set_variable(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    StateKey key{};
    VariableValue value{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    if (!parse_variable_value(state, 3, value)) {
        return luaL_argerror(state, 3, "mission variable value is not a bounded scalar");
    }
    CallFrame& frame = active_frame(state);
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.variables, frame.candidate.variableCount, key, found);
    if (!found) {
        if (frame.candidate.variableCount == frame.candidate.variables.size()) {
            return luaL_error(state, "mission variable capacity exceeded");
        }
        for (std::size_t move = frame.candidate.variableCount; move > row; --move) {
            frame.candidate.variables[move] = frame.candidate.variables[move - 1];
        }
        ++frame.candidate.variableCount;
    }
    frame.candidate.variables[row] = {key, value};
    log_variable_write(key, value);
    return 0;
}
/** Stages one variable removal that commits with the enclosing callback. */
[[nodiscard]] int context_clear_variable(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    CallFrame& frame = active_frame(state);
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.variables, frame.candidate.variableCount, key, found);
    if (found) {
        for (std::size_t move = row + 1; move < frame.candidate.variableCount; ++move) {
            frame.candidate.variables[move - 1] = frame.candidate.variables[move];
        }
        --frame.candidate.variableCount;
        frame.candidate.variables[frame.candidate.variableCount] = {};
    }
    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}
namespace {

/** @return The staged row for one flag name, or the count when none is staged. */
[[nodiscard]] std::size_t staged_save_flag(const Candidate& candidate,
                                           const StateKey& key) noexcept {
    const std::string_view selected = state_key_view(key);
    for (std::size_t index = 0; index < candidate.saveFlagCount; ++index) {
        if (state_key_view(candidate.saveFlags[index].name) == selected) {
            return index;
        }
    }
    return candidate.saveFlagCount;
}

/** Logs a flag call the sandbox refuses before it reaches the save. `name` may be empty. */
void log_flag_refusal(const Impl& impl, std::string_view name, const char* reason) noexcept {
    const std::string_view mod(impl.identity.modId.data());
    const std::string_view shownName = name.empty() ? std::string_view("-") : name;
    const std::string_view shownMod = mod.empty() ? std::string_view("-") : mod;
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=mission_script stage=flag result=refused name=%.*s mod=%.*s reason=%s",
                      static_cast<int>(shownName.size()),
                      shownName.data(),
                      static_cast<int>(shownMod.size()),
                      shownMod.data(),
                      reason);
    if (written > 0) {
        core::log::write(
            core::log::Channel::server,
            core::log::Level::warn,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

} // namespace

/**
 * Stages one per-save flag write: set_flag(name, value). A nil value removes the flag.
 * The value is a boolean or an integer; it reaches the save when the callback commits.
 * @return Zero Lua results; a refused call raises a Lua error.
 */
[[nodiscard]] int context_set_flag(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const Impl* const impl = impl_from_state(state);
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        log_flag_refusal(*impl, {}, "invalid_name");
        return luaL_argerror(state, 2, "flag name is invalid");
    }
    SaveFlag flag{};
    flag.name = key;
    const int valueType = lua_type(state, 3);
    if (valueType == LUA_TBOOLEAN) {
        flag.boolean = true;
        flag.value = lua_toboolean(state, 3) != 0 ? 1 : 0;
        flag.present = true;
    } else if (valueType == LUA_TNUMBER && lua_isinteger(state, 3)) {
        flag.value = static_cast<std::int64_t>(lua_tointeger(state, 3));
        flag.present = true;
    } else if (valueType != LUA_TNIL && valueType != LUA_TNONE) {
        log_flag_refusal(*impl, state_key_view(key), "invalid_value");
        return luaL_argerror(state, 3, "flag value must be a boolean, an integer or nil");
    }
    if (impl->identity.writeSaveFlags == nullptr) {
        log_flag_refusal(*impl, state_key_view(key), "no_store");
        return luaL_error(state, "per-save flags are unavailable");
    }
    Candidate& candidate = active_frame(state).candidate;
    const std::size_t row = staged_save_flag(candidate, key);
    if (row == candidate.saveFlags.size()) {
        log_flag_refusal(*impl, state_key_view(key), "capacity");
        return luaL_error(state, "flag writes in one callback exceed their capacity");
    }
    candidate.saveFlags[row] = flag;
    candidate.saveFlagCount = (std::max)(candidate.saveFlagCount, row + 1);
    return 0;
}

/**
 * Reads one per-save flag: get_flag(name), including a write made earlier in this callback.
 * @return One Lua result: the boolean, the integer, or nil when the flag is unset.
 */
[[nodiscard]] int context_get_flag(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const Impl* const impl = impl_from_state(state);
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        log_flag_refusal(*impl, {}, "invalid_name");
        return luaL_argerror(state, 2, "flag name is invalid");
    }
    const Candidate& candidate = active_frame(state).candidate;
    SaveFlag flag{};
    const std::size_t row = staged_save_flag(candidate, key);
    if (row < candidate.saveFlagCount) {
        // A staged write answers the read; the store logs every read it serves.
        flag = candidate.saveFlags[row];
    } else if (impl->identity.readSaveFlag == nullptr) {
        log_flag_refusal(*impl, state_key_view(key), "no_store");
        return luaL_error(state, "per-save flags are unavailable");
    } else if (!impl->identity.readSaveFlag(impl->identity.modId.data(), key, flag)) {
        return luaL_error(state, "per-save flag read failed");
    }
    if (!flag.present) {
        lua_pushnil(state);
    } else if (flag.boolean) {
        lua_pushboolean(state, flag.value != 0 ? 1 : 0);
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(flag.value));
    }
    return 1;
}

/** Arms or replaces one timer at an absolute authoritative deadline. */
[[nodiscard]] int context_start_timer(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid");
    }
    const lua_Integer delay = luaL_checkinteger(state, 3);
    if (delay < 0
        || static_cast<std::uint64_t>(delay) > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_argerror(state, 3, "mission timer delay is outside u32 milliseconds");
    }
    CallFrame& frame = active_frame(state);
    if (frame.candidate.nextTimerSequence
        == ::sunrise::state::activity::mission::kAbsentTimerSequence) {
        return luaL_error(state, "mission timer sequence is exhausted");
    }
    const std::uint64_t sequence = frame.candidate.nextTimerSequence;
    frame.candidate.nextTimerSequence =
        sequence == (std::numeric_limits<std::uint64_t>::max)()
            ? ::sunrise::state::activity::mission::kAbsentTimerSequence
            : sequence + 1;
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.timers, frame.candidate.timerCount, key, found);
    if (!found) {
        if (frame.candidate.timerCount == frame.candidate.timers.size()) {
            return luaL_error(state, "mission timer capacity exceeded");
        }
        for (std::size_t move = frame.candidate.timerCount; move > row; --move) {
            frame.candidate.timers[move] = frame.candidate.timers[move - 1];
        }
        ++frame.candidate.timerCount;
    }
    frame.candidate.timers[row] = {
        key, timer_deadline(frame.now, static_cast<std::uint32_t>(delay)), sequence};
    push_u64_string(state, sequence);
    return 1;
}
/** Cancels one candidate timer, reporting whether it existed. */
[[nodiscard]] int context_cancel_timer(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid");
    }
    CallFrame& frame = active_frame(state);
    bool found = false;
    const std::size_t row =
        find_state_row(frame.candidate.timers, frame.candidate.timerCount, key, found);
    if (found) {
        for (std::size_t move = row + 1; move < frame.candidate.timerCount; ++move) {
            frame.candidate.timers[move - 1] = frame.candidate.timers[move];
        }
        --frame.candidate.timerCount;
        frame.candidate.timers[frame.candidate.timerCount] = {};
    }
    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}
/** Reads one StateView member: phase, revision, or an authorized query. */
[[nodiscard]] int state_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kStateMetatable));
    const Impl* const impl = impl_from_state(state);
    return dispatch<Impl>(state, kStateBindings, lua_string_view(state, 2), *impl);
}

void register_state_metatables(lua_State* state) {
    register_metatable(state, kStateMetatable, &state_index);
}
} // namespace sunrise::server::activity::mission::lua_vm::detail
