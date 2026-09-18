#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

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
    if (!parse_script_timer_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid or reserved");
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
/** Arms or replaces one timer at an absolute authoritative deadline. */
[[nodiscard]] int context_start_timer(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    StateKey key{};
    if (!parse_script_timer_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid or reserved");
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
    if (!parse_script_timer_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid or reserved");
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
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "phase") {
        const CallFrame& frame = active_frame(state);
        lua_pushinteger(state, frame.candidate.phaseChanged ? frame.candidate.phase : impl->phase);
    } else if (key == "revision") {
        push_u64_string(state, impl->stateRevision);
    } else if (key == "variable") {
        lua_pushcfunction(state, &state_variable);
    } else if (key == "has_variable") {
        lua_pushcfunction(state, &state_has_variable);
    } else if (key == "timer") {
        lua_pushcfunction(state, &state_timer);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

void register_state_metatables(lua_State* state) {
    register_metatable(state, kStateMetatable, &state_index);
}
} // namespace sunrise::server::activity::mission::lua_vm::detail
