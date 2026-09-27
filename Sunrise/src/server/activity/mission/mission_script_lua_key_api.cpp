// Mission identity types. Each one is minted from a name or a key the runtime already checked, so
// a script holds the identity instead of spelling it.

#include <cstdint>
#include <string_view>

#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** Compares two keys the runtime minted. Concatenating a decimal string reaches nothing. */
[[nodiscard]] int request_key_matches(lua_State* state) {
    const auto* const handle =
        static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 1, kRequestKeyMetatable));
    const auto* const other =
        static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 2, kRequestKeyMetatable));
    lua_pushboolean(state, handle->key == other->key ? 1 : 0);
    return 1;
}

/** Lua index for one request key: matches and value. */
[[nodiscard]] int request_key_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kRequestKeyMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "matches") {
        lua_pushcfunction(state, &request_key_matches);
    } else if (key == "value") {
        const auto* handle =
            static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 1, kRequestKeyMetatable));
        push_u64_string(state, handle->key);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for one timer reference: its key and name. */
[[nodiscard]] int timer_ref_index(lua_State* state) {
    const auto* const handle =
        static_cast<const TimerRefHandle*>(luaL_checkudata(state, 1, kTimerRefMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        const std::string_view name = state_key_view(handle->key);
        lua_pushlstring(state, name.data(), name.size());
    } else {
        lua_pushnil(state);
    }
    return 1;
}

[[nodiscard]] int timer_ref_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kTimerRefCollectionMetatable));
    StateKey key{};
    if (!parse_script_timer_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid or reserved");
    }
    push_timer_ref(state, key);
    return 1;
}

/** Lua index for the timer reference collection: `resolve` and `capacity`, else nil. */
[[nodiscard]] int timer_ref_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kTimerRefCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "resolve") {
        lua_pushcfunction(state, &timer_ref_collection_resolve);
    } else if (key == "capacity") {
        lua_pushinteger(state, static_cast<lua_Integer>(kTimerCapacity));
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Lua index for one variable reference: its key and name. */
[[nodiscard]] int variable_ref_index(lua_State* state) {
    const auto* const handle =
        static_cast<const VariableRefHandle*>(luaL_checkudata(state, 1, kVariableRefMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "name") {
        const std::string_view name = state_key_view(handle->key);
        lua_pushlstring(state, name.data(), name.size());
    } else {
        lua_pushnil(state);
    }
    return 1;
}

[[nodiscard]] int variable_ref_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kVariableRefCollectionMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    push_variable_ref(state, key);
    return 1;
}

/** Lua index for the variable reference collection: `resolve` and `capacity`, else nil. */
[[nodiscard]] int variable_ref_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kVariableRefCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (key == "resolve") {
        lua_pushcfunction(state, &variable_ref_collection_resolve);
    } else if (key == "capacity") {
        lua_pushinteger(state, static_cast<lua_Integer>(kVariableCapacity));
    } else {
        lua_pushnil(state);
    }
    return 1;
}

} // namespace

void push_request_key(lua_State* state, std::uint64_t key) {
    push_handle(state, kRequestKeyMetatable, RequestKeyHandle{key});
}

void push_timer_ref(lua_State* state, const StateKey& key) {
    push_handle(state, kTimerRefMetatable, TimerRefHandle{key});
}

void push_variable_ref(lua_State* state, const StateKey& key) {
    push_handle(state, kVariableRefMetatable, VariableRefHandle{key});
}

void register_key_metatables(lua_State* state) {
    register_metatable(state, kRequestKeyMetatable, &request_key_index);
    register_metatable(state, kTimerRefMetatable, &timer_ref_index);
    register_metatable(state, kTimerRefCollectionMetatable, &timer_ref_collection_index);
    register_metatable(state, kVariableRefMetatable, &variable_ref_index);
    register_metatable(state, kVariableRefCollectionMetatable, &variable_ref_collection_index);
}

/** Pushes one recognized key-context member. @return False when the key is not ours. */
bool push_key_context_member(lua_State* state, std::string_view key) {
    if (key == "timers") {
        push_handle(state, kTimerRefCollectionMetatable, TimerRefCollectionHandle{});
        return true;
    }
    if (key == "variables") {
        push_handle(state, kVariableRefCollectionMetatable, VariableRefCollectionHandle{});
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
