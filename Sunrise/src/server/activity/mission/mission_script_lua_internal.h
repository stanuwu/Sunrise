#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../../../state/activity_sdk/runtime.h"
#include "mission_script_lua_args.h"
#include "mission_script_lua_bounded_internal.h"
#include "mission_script_lua_enum_internal.h"
#include "mission_script_lua_key_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_peer_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_surface.h"
#include "mission_script_lua_types.h"
#include "mission_script_lua_value_internal.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

// Every mission API entry point, grouped by the translation unit that defines it. The names,
// handles, re-read helpers and surface matchers live in the headers included above.

/** Decodes the generated SDK build identity into its durable digest. */
[[nodiscard]] inline bool decode_sdk_build_sha256(std::string_view text,
                                                  std::array<std::byte, 32>& output) noexcept {
    output = {};
    // The generated SDK build identity is spelled as this prefix and 64 hex digits.
    constexpr std::string_view kPrefix = "sha256:";
    if (!text.starts_with(kPrefix)) {
        return false;
    }
    text.remove_prefix(kPrefix.size());
    if (text.size() != output.size() * 2U) {
        return false;
    }
    const auto nibble = [](char value, std::uint8_t& result) noexcept {
        if (value >= '0' && value <= '9') {
            result = static_cast<std::uint8_t>(value - '0');
            return true;
        }
        if (value >= 'a' && value <= 'f') {
            result = static_cast<std::uint8_t>(value - 'a' + 10);
            return true;
        }
        if (value >= 'A' && value <= 'F') {
            result = static_cast<std::uint8_t>(value - 'A' + 10);
            return true;
        }
        return false;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!nibble(text[index * 2U], high) || !nibble(text[index * 2U + 1U], low)) {
            output = {};
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4U) | low);
    }
    return true;
}

/**
 * Queues one built intent on the pending candidate and returns its RequestKey.
 * It owns the allocation failure, so no effect repeats one.
 * @return The Lua result count.
 */
[[nodiscard]] inline int queue_intent(lua_State* state, CallFrame& frame, Intent& intent) {
    intent.requestKey = mint_intent_key(frame.candidate);
    if (intent.requestKey == state::activity::mission::kAbsentIntentKey) {
        return luaL_error(state, "mission request keys are exhausted");
    }
    const std::uint64_t requestKey = intent.requestKey;
    try {
        frame.candidate.intents.push_back(std::move(intent));
    } catch (const std::bad_alloc&) {
        frame.intentAllocationFailed = true;
        return luaL_error(state, "mission intent allocation failed");
    }
    push_request_key(state, requestKey);
    return 1;
}

// Metatable registration, one function per domain. setup_sandbox calls these and holds no
// per-type list of its own.

void register_context_metatables(lua_State* state);
void register_definition_metatables(lua_State* state);
void register_collection_metatables(lua_State* state);
void register_state_metatables(lua_State* state);
void register_lifetime_metatables(lua_State* state);
void register_event_metatables(lua_State* state);
void register_squad_metatables(lua_State* state);
void register_scene_metatables(lua_State* state);
void register_slot_metatables(lua_State* state);
void register_actor_sequence_metatables(lua_State* state);
[[nodiscard]] int slot_actor_sequences(lua_State* state);
[[nodiscard]] int slot_play_actor_sequence(lua_State* state);

// SDK definition readers, defined in mission_script_lua_definition_api.cpp.

[[nodiscard]] int message_index(lua_State* state);
[[nodiscard]] int message_field_index(lua_State* state);
[[nodiscard]] int activity_binding_locator_index(lua_State* state);
[[nodiscard]] int activity_index(lua_State* state);

// The sandbox and the mission callback frame, defined in mission_script_lua_sandbox.cpp.
// initialize_sandbox, protected_callback, set_error and destroy_state are declared in
// mission_script_vm_internal.h.

// Row collections, defined in mission_script_lua_collection_api.cpp.

[[nodiscard]] int activity_binding_tag_collection_index(lua_State* state);
[[nodiscard]] int activity_binding_locator_collection_index(lua_State* state);
[[nodiscard]] int squad_collection_index(lua_State* state);
[[nodiscard]] int scene_collection_index(lua_State* state);
[[nodiscard]] int slot_collection_index(lua_State* state);
[[nodiscard]] int message_collection_index(lua_State* state);
[[nodiscard]] int message_field_collection_index(lua_State* state);

// The running callback's activity binding, defined in mission_script_lua_context_api.cpp.

[[nodiscard]] int context_index(lua_State* state);

// Typed effects, each defined beside the handle it needs.

[[nodiscard]] int squad_index(lua_State* state);
[[nodiscard]] int squad_place(lua_State* state);
[[nodiscard]] int squad_actor_command(lua_State* state);
[[nodiscard]] int scene_index(lua_State* state);
[[nodiscard]] int scene_activate(lua_State* state);
[[nodiscard]] int scene_send_event(lua_State* state);
[[nodiscard]] int scene_stop(lua_State* state);
[[nodiscard]] int lifetime_index(lua_State* state);
[[nodiscard]] int lifetime_set(lua_State* state);

// Mission context, variables and timers, defined in mission_script_lua_state_api.cpp.

[[nodiscard]] int context_set_phase(lua_State* state);
[[nodiscard]] int context_select_state(lua_State* state);
[[nodiscard]] int context_set_variable(lua_State* state);
[[nodiscard]] int context_clear_variable(lua_State* state);
[[nodiscard]] int context_start_timer(lua_State* state);
[[nodiscard]] int context_cancel_timer(lua_State* state);
[[nodiscard]] int state_index(lua_State* state);

// Message matching, defined in mission_script_lua_message_api.cpp.

[[nodiscard]] const char* lua_client_message_status_name(host::ClientMessageStatus status) noexcept;
[[nodiscard]] int message_matches(lua_State* state);

/** Sets one verified device channel through the same guarded route. */
[[nodiscard]] int slot_set_channel(lua_State* state);

/** Applies one named device transition over the same guarded channel route. */
[[nodiscard]] int slot_transition(lua_State* state);

/** Sets the object filter and caller value one occupancy condition carries. */
[[nodiscard]] int slot_set_occupancy_condition(lua_State* state);

/** Shows one generated type-68 HUD directive. */
[[nodiscard]] int slot_set_directive(lua_State* state);

/** Hides the active type-68 HUD directive. */
[[nodiscard]] int slot_clear_directives(lua_State* state);

/** Reads one member of a Slot row. */
[[nodiscard]] int slot_index(lua_State* state);

/** @return True when one live Slot row is an exact type-2 combatant. */
[[nodiscard]] bool exact_combatant_slot(const SlotDefinition& definition) noexcept;

/** Stages one authored native Auth body on the guarded message-5 route. */
[[nodiscard]] int queue_slot_auth(lua_State* state,
                                  const SlotDefinition& slot,
                                  std::uint32_t schema,
                                  std::size_t bitCount,
                                  std::span<const std::byte> body);

/** Reads one integer field from a generated declaration table. */
[[nodiscard]] lua_Integer directive_integer(lua_State* state, int table, const char* field);

// The client-atom program, defined in mission_script_lua_slot_atoms.cpp.

/** Loads the 32-lane client-atom program one type-2 combatant runs on its bound actor. */
[[nodiscard]] int slot_run_atoms(lua_State* state);

} // namespace sunrise::server::activity::mission::lua_vm::detail
