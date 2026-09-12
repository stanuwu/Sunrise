#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string_view>

#include "mission_script_lua_bap_internal.h"
#include "mission_script_lua_catalog_internal.h"
#include "mission_script_lua_event_internal.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_manifest_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_types.h"
#include "mission_script_lua_world_internal.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** Refuses collection-sensitive metamethods and installs a hidden copy the caller cannot mutate. */
[[nodiscard]] int safe_setmetatable(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    if (lua_isnoneornil(state, 2)) {
        lua_pushvalue(state, 1);
        lua_pushnil(state);
        lua_setmetatable(state, -2);
        return 1;
    }
    luaL_checktype(state, 2, LUA_TTABLE);
    // A metatable a script installs may not run code or weaken a reference at collection.
    constexpr std::array<const char*, 3> refused{"__gc", "__mode", "__close"};
    for (const char* const key : refused) {
        lua_pushstring(state, key);
        lua_rawget(state, 2);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (present) {
            return luaL_error(state, "collection-sensitive metamethod is unavailable");
        }
    }
    // Apply an unreachable copy. The caller may keep and mutate its source table through rawset,
    // but it can never add collection-sensitive entries to the installed metatable afterward.
    lua_newtable(state);
    const int installed = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, 2) != 0) {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_rawset(state, installed);
        lua_pop(state, 1);
    }
    lua_pushliteral(state, "__metatable");
    lua_pushboolean(state, 0);
    lua_rawset(state, installed);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, installed);
    lua_setmetatable(state, -2);
    return 1;
}

/** Preserves base pcall semantics while making the instruction-budget error uncatchable. */
[[nodiscard]] int safe_pcall(lua_State* state) {
    const int arguments = lua_gettop(state) - 1;
    luaL_checktype(state, 1, LUA_TFUNCTION);
    const int status = lua_pcall(state, arguments, LUA_MULTRET, 0);
    CallFrame& frame = active_frame(state);
    if (status == LUA_ERRMEM || status == LUA_ERRERR) {
        frame.memoryExceeded = true;
    }
    if (frame.instructionExceeded || frame.memoryExceeded || frame.intentAllocationFailed) {
        if (status == LUA_OK) {
            lua_pushliteral(state, "instruction_budget");
        }
        return lua_error(state);
    }
    lua_pushboolean(state, status == LUA_OK ? 1 : 0);
    lua_insert(state, 1);
    return lua_gettop(state);
}

void remove_global(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setglobal(state, name);
}

/** Allows only generated dotted module names below the one configured SDK directory. */
[[nodiscard]] int safe_require(lua_State* state) {
    std::size_t length = 0;
    const char* const name = luaL_checklstring(state, 1, &length);
    if (length == 0 || name[0] == '.' || name[length - 1] == '.') {
        return luaL_error(state, "generated SDK module name is invalid");
    }
    bool previousDot = false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(name[index]);
        const bool dot = byte == '.';
        if ((!dot && byte != '_' && std::isalnum(byte) == 0) || (dot && previousDot)) {
            return luaL_error(state, "generated SDK module name is invalid");
        }
        previousDot = dot;
    }
    lua_settop(state, 1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, 1, LUA_MULTRET);
    return lua_gettop(state);
}

/** Opens only the Lua-file package searcher and hides package mutation from authored code. */
void configure_generated_require(lua_State* state, const ProgramIdentity& identity) {
    if (identity.sdkLuaSearchPath[0] == '\0') {
        return;
    }
    luaL_requiref(state, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pushstring(state, identity.sdkLuaSearchPath.data());
    lua_setfield(state, -2, "path");
    lua_pushliteral(state, "");
    lua_setfield(state, -2, "cpath");
    lua_pushnil(state);
    lua_setfield(state, -2, "loadlib");
    lua_getfield(state, -1, "searchers");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        lua_rawseti(state, -2, 3);
        lua_pushnil(state);
        lua_rawseti(state, -2, 4);
    }
    lua_pop(state, 2);

    lua_getglobal(state, "require");
    lua_pushcclosure(state, &safe_require, 1);
    lua_setglobal(state, "require");
    remove_global(state, LUA_LOADLIBNAME);
}

/** Opens the allowed libraries, removes unsafe globals, and registers every handle metatable. */
[[nodiscard]] int setup_sandbox(lua_State* state) {
    Impl* const impl = impl_from_state(state);
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    configure_generated_require(state, impl->identity);

    // Globals a sandboxed program may not reach: host IO, collection control, and any
    // iteration or protected call whose cost the instruction budget cannot charge.
    constexpr std::array<const char*, 13> removedGlobals{
        "collectgarbage",
        "dofile",
        "getmetatable",
        "load",
        "loadfile",
        "next",
        "pairs",
        "pcall",
        "print",
        "rawequal",
        "rawlen",
        "warn",
        "xpcall",
    };
    for (const char* const name : removedGlobals) {
        remove_global(state, name);
    }
    lua_pushcfunction(state, &safe_setmetatable);
    lua_setglobal(state, "setmetatable");
    lua_pushcfunction(state, &safe_pcall);
    lua_setglobal(state, "pcall");

    lua_getglobal(state, LUA_STRLIBNAME);
    // String functions whose pattern matching has no instruction bound.
    constexpr std::array<const char*, 5> removedStringFunctions{
        "dump", "find", "match", "gmatch", "gsub"};
    for (const char* const name : removedStringFunctions) {
        lua_pushnil(state);
        lua_setfield(state, -2, name);
    }
    lua_pop(state, 1);
    lua_getglobal(state, LUA_MATHLIBNAME);
    lua_pushnil(state);
    lua_setfield(state, -2, "random");
    lua_pushnil(state);
    lua_setfield(state, -2, "randomseed");
    lua_pop(state, 1);

    // One call per domain. A domain owns its own metatable list, so adding a type never edits
    // this function.
    register_context_metatables(state);
    register_definition_metatables(state);
    register_collection_metatables(state);
    register_state_metatables(state);
    register_lifetime_metatables(state);
    register_event_metatables(state);
    register_squad_metatables(state);
    register_scene_metatables(state);
    register_slot_metatables(state);
    register_actor_sequence_metatables(state);
    register_peer_metatables(state);
    register_bap_metatables(state);
    register_value_metatables(state);
    register_enum_metatables(state);
    register_key_metatables(state);
    register_bounded_metatables(state);
    catalog_api::register_metatables(state);
    manifest_api::register_metatables(state);
    world_api::register_metatables(state);
    return 0;
}

void push_context(lua_State* state) {
    push_handle(state, kContextMetatable, ContextHandle{});
}

void push_state(lua_State* state) {
    push_handle(state, kStateMetatable, StateHandle{});
}

void push_event(lua_State* state, const host::Event& source) {
    push_handle(state, event_metatable(source.kind), EventHandle{source});
}

} // namespace

/** Copies one sanitized diagnostic and never allocates. */
void set_error(Impl& impl, std::string_view error) noexcept {
    impl.lastError = {};
    const std::size_t length = (std::min)(error.size(), impl.lastError.size() - 1);
    for (std::size_t index = 0; index < length; ++index) {
        const char value = error[index];
        // The log line quotes this text, so a double quote here would end the field early.
        const bool blank = value == '\r' || value == '\n' || value == '\t';
        impl.lastError[index] = blank ? ' ' : value == '"' ? '\'' : value;
    }
}

/** Builds the sandbox from additions while setup itself remains protected. */
bool initialize_sandbox(Impl& impl) noexcept {
    attach_impl(impl.state, impl);
    lua_pushcfunction(impl.state, &setup_sandbox);
    const int result = lua_pcall(impl.state, 0, 0, 0);
    if (result == LUA_OK) {
        return true;
    }
    const char* const error =
        lua_type(impl.state, -1) == LUA_TSTRING ? lua_tostring(impl.state, -1) : nullptr;
    set_error(impl,
              error == nullptr ? "sandbox initialization failed with a non-string error" : error);
    lua_settop(impl.state, 0);
    return false;
}

/** Executes one already-selected entry; all argument allocation stays under lua_pcall. */
int protected_callback(lua_State* state) {
    Impl* const impl = impl_from_state(state);
    CallFrame& frame = active_frame(state);
    const int reference = frame.handler == Handler::event && frame.event != nullptr
                              ? event_reference(*impl, frame.event->kind)
                              : handler_reference(*impl, frame.handler);
    if (reference == LUA_NOREF) {
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    if (!lua_isfunction(state, -1)) {
        return luaL_error(state, "mission entry is no longer a function");
    }
    push_context(state);
    push_state(state);
    int arguments = 2;
    if (frame.handler == Handler::event) {
        if (frame.event == nullptr) {
            return luaL_error(state, "mission event is missing");
        }
        push_event(state, *frame.event);
        arguments = 3;
    }
    lua_call(state, arguments, 0);
    lua_pushboolean(state, 1);
    return 1;
}

/** Closes Lua before resetting the allocator that owns its objects. */
void destroy_state(Impl& impl) noexcept {
    if (impl.state != nullptr) {
        lua_close(impl.state);
        impl.arenaBytesAfterClose = impl.arena.used;
    }
    impl.identity = {};
    impl.definitions = {};
    std::vector<Intent>{}.swap(impl.outbox);
    impl.variables = {};
    impl.timers = {};
    impl.peers = {};
    impl.peerCount = 0;
    impl.outboxRead = 0;
    impl.variableCount = 0;
    impl.timerCount = 0;
    impl.nextTimerSequence = state::activity::mission::kFirstTimerSequence;
    impl.stateRevision = 0;
    impl.callbacks = 0;
    impl.committedCallbacks = 0;
    impl.refusedCallbacks = 0;
    impl.collections = 0;
    impl.phase = 0;
    impl.initialStateRegion = -1;
    impl.lastError = {};
    impl.state = nullptr;
    impl.frame = nullptr;
    impl.programReference = LUA_NOREF;
    impl.startReference = LUA_NOREF;
    impl.eventReferences.fill(LUA_NOREF);
    impl.loadReference = LUA_NOREF;
    impl.hasInitialState = false;
    impl.active = false;
    impl.faulted = false;
    arena_release(impl.arena);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
