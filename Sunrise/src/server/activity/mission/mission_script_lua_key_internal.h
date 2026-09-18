#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

// Metatable names that lock the key, timer and variable userdata shapes a program may hold.
inline constexpr char kRequestKeyMetatable[] = "sunrise.mission.request_key";
inline constexpr char kTimerRefMetatable[] = "sunrise.mission.timer_ref";
inline constexpr char kTimerRefCollectionMetatable[] = "sunrise.mission.timers";
inline constexpr char kVariableRefMetatable[] = "sunrise.mission.variable_ref";
inline constexpr char kVariableRefCollectionMetatable[] = "sunrise.mission.variables";

/**
 * One effect correlation key. It has no numeric or string accessor, so a script can only compare
 * a key an effect gave it against a key an event reports.
 */
struct RequestKeyHandle final {
    std::uint64_t key{};
};

/** One parsed timer name. The name passed parse_state_key at the mint. */
struct TimerRefHandle final {
    StateKey key{};
};

struct TimerRefCollectionHandle final {
    std::uint8_t marker{};
};

/** One parsed variable name. The name passed parse_state_key at the mint. */
struct VariableRefHandle final {
    StateKey key{};
};

struct VariableRefCollectionHandle final {
    std::uint8_t marker{};
};

/** @return The stored name of one parsed key. */
[[nodiscard]] inline std::string_view state_key_view(const StateKey& key) noexcept {
    return {key.bytes.data(), key.length};
}

/**
 * Reads one bounded variable or timer name from the Lua stack.
 * Nothing here can throw, so an inlined copy inside a Lua reader cannot unwind into terminate.
 */
[[nodiscard]] inline bool parse_state_key(lua_State* state, int index, StateKey& output) noexcept {
    output = {};
    if (lua_type(state, index) != LUA_TSTRING) {
        return false;
    }
    std::size_t length = 0;
    const char* const value = lua_tolstring(state, index, &length);
    if (value == nullptr || length == 0 || length >= output.bytes.size()) {
        return false;
    }
    for (std::size_t cursor = 0; cursor < length; ++cursor) {
        const unsigned char byte = static_cast<unsigned char>(value[cursor]);
        const bool alphaNumeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
                                  || (byte >= '0' && byte <= '9');
        if (!alphaNumeric && byte != '_' && byte != '-' && byte != '.' && byte != '/') {
            return false;
        }
    }
    std::copy_n(value, length, output.bytes.data());
    output.length = static_cast<std::uint8_t>(length);
    return true;
}

/**
 * Reads one timer name a script supplied, refusing the runtime's reserved prefix.
 * @return False when the name is malformed or reserved.
 */
[[nodiscard]] inline bool
parse_script_timer_key(lua_State* state, int index, StateKey& output) noexcept {
    return parse_state_key(state, index, output) && !runtime_timer_key(output);
}

void push_request_key(lua_State* state, std::uint64_t key);
void push_timer_ref(lua_State* state, const StateKey& key);
void push_variable_ref(lua_State* state, const StateKey& key);

/** Registers every locked mission-key userdata shape. */
void register_key_metatables(lua_State* state);

/** Pushes one recognized Context key member and returns whether the key was owned. */
[[nodiscard]] bool push_key_context_member(lua_State* state, std::string_view key);

} // namespace sunrise::server::activity::mission::lua_vm::detail
