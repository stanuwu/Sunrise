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

int context_cohort(lua_State* state);
void register_population_metatables(lua_State* state);
/**
 * Reads a squad's temperament list from argument table field `key` into the intent.
 * @return False when the call gave none, so the squad keeps its last list.
 */
/**
 * Reads an optional list of mission.ActorTemperament rows of one slot, at most an actor's capacity.
 * @param slotRow The actor or squad slot the rows must belong to.
 * @return False when the call gave no list.
 */
[[nodiscard]] bool read_temperament_list(
    lua_State* state,
    const char* key,
    std::uint32_t slotRow,
    std::array<std::uint32_t,
               middleware::bap::activity_message::scriptable_auth::kType2TemperamentCapacity>&
        output,
    std::uint8_t& count);
[[nodiscard]] bool
read_squad_temperaments(lua_State* state, const char* key, std::uint32_t slotRow, Intent& intent);

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
    intent.attemptGeneration = impl_from_state(state)->attempt.generation;
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
[[nodiscard]] int slot_applied(lua_State* state);

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
/** Sets an SDK-bounded prefix of authored object output channels. */

/** Applies one named device transition over the same guarded channel route. */
[[nodiscard]] int slot_transition(lua_State* state);

/** Sets the object filter and caller value one occupancy condition carries. */
[[nodiscard]] int slot_set_occupancy_condition(lua_State* state);

/** Shows one generated type-68 HUD directive. */
[[nodiscard]] int slot_set_directive(lua_State* state);

/** Configures the authored volumes that admit players to a type-70 audience. */
[[nodiscard]] int slot_set_engagement(lua_State* state);

/** Hides the active type-68 HUD directive. */
[[nodiscard]] int slot_clear_directives(lua_State* state);

/** Reads one member of a Slot row. */
[[nodiscard]] int slot_index(lua_State* state);

/** @return True when one live Slot row is an exact type-2 combatant. */
[[nodiscard]] bool exact_combatant_slot(const SlotDefinition& definition) noexcept;

/** @return True for an exact authored type-58 path of the actor's own object and registry. */
[[nodiscard]] bool exact_actor_path(const SlotDefinition& path,
                                    const SlotDefinition& actor) noexcept;

/** Stages one authored native Auth body on the guarded message-5 route. */
[[nodiscard]] int queue_slot_auth(lua_State* state,
                                  const SlotDefinition& slot,
                                  std::uint32_t schema,
                                  std::size_t bitCount,
                                  std::span<const std::byte> body,
                                  IntentKind kind = IntentKind::applySlotAuth,
                                  bool active = false,
                                  bool objectInPlace = false);
void push_atom_kinds(lua_State* state);

/** Reads one integer field from a generated declaration table. */
[[nodiscard]] lua_Integer directive_integer(lua_State* state, int table, const char* field);
[[nodiscard]] int slot_assign_combat_objective(lua_State* state);
[[nodiscard]] int slot_task_counter(lua_State* state);
[[nodiscard]] int event_task_cost(lua_State* state);
[[nodiscard]] int event_task_group(lua_State* state);

[[nodiscard]] int slot_set_music_section(lua_State* state);
[[nodiscard]] int slot_set_public_event_state(lua_State* state);
[[nodiscard]] int slot_play_sequence(lua_State* state);
[[nodiscard]] int slot_set_cinematic_active(lua_State* state);
[[nodiscard]] int slot_reset_objectives(lua_State* state);
[[nodiscard]] int slot_advance_task(lua_State* state);
[[nodiscard]] int slot_play_performance(lua_State* state);
[[nodiscard]] int slot_play_dialogue_cue(lua_State* state);
[[nodiscard]] int slot_play_actor_path(lua_State* state);
[[nodiscard]] int slot_play_actor_action(lua_State* state);
[[nodiscard]] int slot_retire_actor(lua_State* state);
[[nodiscard]] int slot_deliver_passengers(lua_State* state);
/** @return The authored point parameters of one slot, counted from 0 until one is missing. */
[[nodiscard]] std::size_t authored_point_count(lua_State* state, const SlotDefinition& slot);
[[nodiscard]] int slot_bind_combatant_to_squad(lua_State* state);
[[nodiscard]] int slot_set_darkness_zone(lua_State* state);
[[nodiscard]] int slot_set_object_filter(lua_State* state);
[[nodiscard]] int slot_generate_map(lua_State* state);
[[nodiscard]] int slot_watch_damage(lua_State* state);
/** Queues the authored type-26 hop-on after validating its filter and revision. */
[[nodiscard]] int slot_set_interactable_object(lua_State* state);
[[nodiscard]] int slot_set_ghost_link(lua_State* state);
[[nodiscard]] int slot_ghost_link(lua_State* state);
[[nodiscard]] int slot_set_object_active(lua_State* state);
/** Sets the verified prefix of an object's authored channels. */
[[nodiscard]] int slot_set_object_channels(lua_State* state);
[[nodiscard]] int slot_fire_trigger(lua_State* state);
[[nodiscard]] int slot_disarm_trigger(lua_State* state);
/** @return True for a positive counter accepted by every typed Auth body. */
[[nodiscard]] bool valid_counter(lua_Integer value) noexcept;
/** Reads an optional typed slot reference from the named argument table. */
[[nodiscard]] bool optional_slot_reference(
    lua_State* state,
    const char* name,
    std::uint32_t slotType,
    middleware::bap::activity_message::scriptable_auth::Type2LaneClientRef& output);

// The client-atom program, defined in mission_script_lua_slot_atoms.cpp.

/** Loads the 32-lane client-atom program one type-2 combatant runs on its bound actor. */
[[nodiscard]] int slot_run_atoms(lua_State* state);
/** Replaces the controls of one live type-2 combatant. */
[[nodiscard]] int slot_set_actor_controls(lua_State* state);

} // namespace sunrise::server::activity::mission::lua_vm::detail
