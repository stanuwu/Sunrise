#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/auth_fields.h"
#include "../../../middleware/bap/activity_message/auth_schema_catalog.h"
#include "../../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../../middleware/bap/activity_message/squad_attachment_auth.h"
#include "../../../middleware/bap/activity_message/volume_toggle_auth.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace auth_fields = middleware::bap::activity_message::auth_fields;

namespace auth_catalog = middleware::bap::activity_message::auth_schema_catalog;

/** Positive 31-bit generations and revisions are the counters every typed Auth body accepts. */
[[nodiscard]] bool valid_counter(lua_Integer value) noexcept {
    return value > 0 && value <= auth_fields::kMaximumCounter;
}

/**
 * Reads one optional slot-handle argument into a ClientRef.
 * @param slotType Slot type the referenced slot must have.
 * @param output Left unset when the argument is nil.
 * @return False when the argument is present but stale or of another type.
 */
[[nodiscard]] bool optional_slot_reference(lua_State* state,
                                           const char* name,
                                           std::uint32_t slotType,
                                           scriptable_auth::Type2LaneClientRef& output) {
    lua_getfield(state, 2, name);
    bool valid = true;
    if (!lua_isnil(state, -1)) {
        const auto* const handle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition definition{};
        valid = current_slot(state, *handle, definition) && definition.slotType == slotType
                && definition.slotIndex <= auth_fields::kMaximumClientRefIndex;
        if (valid) {
            output = {definition.registryKey,
                      static_cast<std::int8_t>(slotType),
                      static_cast<std::int16_t>(definition.slotIndex)};
        }
    }
    lua_pop(state, 1);
    return valid;
}

/** Stages one authored native Auth body on the guarded message-5 route. */
[[nodiscard]] int queue_slot_auth(lua_State* state,
                                  const SlotDefinition& slot,
                                  std::uint32_t schema,
                                  std::size_t bitCount,
                                  std::span<const std::byte> body,
                                  IntentKind kind,
                                  bool active) {
    Impl* const impl = impl_from_state(state);
    std::array<std::byte, 32> sdkBuildSha256{};
    if (!decode_sdk_build_sha256(std::string_view(impl->identity.sdkBuildId.data()),
                                 sdkBuildSha256)) {
        return luaL_error(state, "loaded SDK generation identity is invalid");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = kind;
    intent.active = active;
    intent.sdkBuildSha256 = sdkBuildSha256;
    intent.firstRow = slot.nativeRow;
    if (kind == IntentKind::runActorProgram && active) {
        if (impl->definitions.resolveActorProgramSource == nullptr
            || !impl->definitions.resolveActorProgramSource(
                impl->definitions.context, slot.nativeRow, intent.secondRow)) {
            return luaL_error(state, "actor creation has no exact authored source");
        }
    }
    intent.objectTag = slot.objectTag;
    intent.registryKey = slot.registryKey;
    intent.authSchema = schema;
    intent.authBitCount = static_cast<std::uint16_t>(bitCount);
    intent.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    intent.slotType = static_cast<std::uint8_t>(slot.slotType);
    intent.authByteCount = static_cast<std::uint16_t>(body.size());
    try {
        intent.authBody.assign(body.begin(), body.end());
    } catch (const std::bad_alloc&) {
        return luaL_error(state, "mission Auth body allocation failed");
    }
    return queue_intent(state, frame, intent);
}

/** Reads one integer field from a generated directive declaration. */
[[nodiscard]] lua_Integer directive_integer(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const lua_Integer value = luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    return value;
}

/** Sends a native type-32 toggle for one current same-owner authored volume. */
[[nodiscard]] int slot_set_volume_active(lua_State* state) {
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> declared{"volume", "active"};
    refuse_unknown_arguments(state, declared);
    const SlotHandle target = checked_argument<SlotHandle>(state, "volume", kSlotMetatable);
    lua_getfield(state, 2, "active");
    if (!lua_isboolean(state, -1)) {
        return luaL_error(state, "active must be a boolean");
    }
    const bool active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    SlotDefinition source{}, volume{};
    if (!current_slot(state, *handle, source) || !current_slot(state, target, volume)) {
        return luaL_error(state, "toggle or volume is stale or invalid");
    }
    if (source.slotType != scriptable_auth::kType32SlotType
        || source.componentClass != scriptable_auth::kType32ComponentClass
        || source.authSchema != scriptable_auth::kType32Schema
        || (source.flags & format::kSlotSchemaJoinExact) == 0) {
        return luaL_error(state, "source must be an exact type-32 toggle sensor");
    }
    if (volume.slotType != scriptable_auth::kVolumeSlotType || volume.objectTag != source.objectTag
        || volume.registryKey != source.registryKey
        || volume.slotIndex
               > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
        return luaL_error(state, "volume must be a same-owner type-60 slot");
    }
    std::array<std::byte, scriptable_auth::kType32ByteCount> body{};
    std::size_t written = 0;
    if (!scriptable_auth::encode_type32_volume(
            {volume.registryKey, static_cast<std::int16_t>(volume.slotIndex), active},
            body,
            written)) {
        return luaL_error(state, "native toggle encoder rejected the volume");
    }
    return queue_slot_auth(
        state, source, scriptable_auth::kType32Schema, scriptable_auth::kType32BitCount, body);
}

/** Selects an audited same-owner squad lifetime for a type-26 attachment. */
[[nodiscard]] int slot_set_squad_attachment(lua_State* state) {
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> declared{
        "source", "spawn_generation", "active"};
    refuse_unknown_arguments(state, declared);
    const SlotHandle sourceHandle = checked_argument<SlotHandle>(state, "source", kSlotMetatable);
    const lua_Integer generation = checked_integer_argument(state, "spawn_generation");
    lua_getfield(state, 2, "active");
    if (!lua_isboolean(state, -1)) {
        return luaL_error(state, "active must be a boolean");
    }
    const bool active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    SlotDefinition target{}, source{};
    if (!current_slot(state, *handle, target) || !current_slot(state, sourceHandle, source)) {
        return luaL_error(state, "attachment or squad source is stale or invalid");
    }
    if (target.slotType != 26 || target.authSchema != scriptable_auth::kType26Schema
        || (target.flags & format::kSlotSchemaJoinExact) == 0
        || source.slotType != format::kSquadSlotType
        || source.authSchema != format::kSquadAuthSchema
        || (source.flags & format::kSlotSchemaJoinExact) == 0
        || target.objectTag != source.objectTag || target.registryKey != source.registryKey) {
        return luaL_error(state, "attachment requires an exact same-owner type-1 source");
    }
    if (generation <= 0 || generation > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "spawn_generation must be a positive signed 32-bit integer");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setSquadAttachment;
    intent.firstRow = target.nativeRow;
    intent.secondRow = source.nativeRow;
    intent.sourceSpawnGeneration = static_cast<std::uint64_t>(generation);
    intent.active = active;
    return queue_intent(state, frame, intent);
}

/** Attaches the authored hop-on effect to the entities a type-34 filter selects. */
[[nodiscard]] int slot_set_mission_effect(lua_State* state) {
    namespace effect = middleware::bap::activity_message::mission_effect;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{"filter", "enabled", "revision"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != effect::kSlotType
        || slot.authSchema != effect::kSchema) {
        return luaL_error(state, "mission effect requires an authored type-26 hop-on");
    }
    const bool enabled = optional_boolean_argument(state, "enabled", true);
    scriptable_auth::Type2LaneClientRef filter{};
    if (enabled) {
        lua_getfield(state, 2, "filter");
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!present
            || !optional_slot_reference(
                state, "filter", scriptable_auth::kType34SlotType, filter)) {
            return luaL_error(state, "mission effect requires a type-34 filter");
        }
    }
    const lua_Integer revision = optional_integer_argument(state, "revision", 1);
    if (!valid_counter(revision)) {
        return luaL_error(state, "effect revision must be positive");
    }
    std::array<std::byte, effect::kBytes> body{};
    std::size_t written = 0;
    if (!effect::encode(filter, enabled, static_cast<std::int32_t>(revision), body, written)) {
        return luaL_error(state, "mission effect encoder failed");
    }
    return queue_slot_auth(state, slot, effect::kSchema, effect::kBits, body);
}
/** Reads one Slot row member, its syntax methods, and its authorized actions. */
[[nodiscard]] int slot_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition)) {
        return luaL_error(state, "activity slot is stale");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "row") {
        lua_pushinteger(state, definition.localRow);
    } else if (key == "applied") {
        lua_pushcfunction(state, &slot_applied);
    } else if (key == "id") {
        lua_pushlstring(state, definition.id.data(), definition.id.size());
    } else if (key == "name") {
        lua_pushlstring(state, definition.name.data(), definition.name.size());
    } else if (key == "object_id") {
        lua_pushlstring(state, definition.objectId.data(), definition.objectId.size());
    } else if (key == "object_tag") {
        lua_pushinteger(state, definition.objectTag);
    } else if (key == "registry_key") {
        lua_pushinteger(state, definition.registryKey);
    } else if (key == "slot_index") {
        lua_pushinteger(state, definition.slotIndex);
    } else if (key == "slot_type") {
        lua_pushinteger(state, definition.slotType);
    } else if (key == "component_class") {
        lua_pushinteger(state, definition.componentClass);
    } else if (key == "sense_schema") {
        lua_pushinteger(state, definition.senseSchema);
    } else if (key == "auth_schema") {
        lua_pushinteger(state, definition.authSchema);
    } else if (key == "sense_schema_id") {
        lua_pushlstring(state, definition.senseSchemaId.data(), definition.senseSchemaId.size());
    } else if (key == "auth_schema_id") {
        lua_pushlstring(state, definition.authSchemaId.data(), definition.authSchemaId.size());
    } else if (key == "auth_type" || key == "auth_min_bits" || key == "auth_max_bits"
               || key == "auth_component_offset" || key == "auth_dynamic"
               || key == "auth_writable") {
        const auth_catalog::Type* const auth = auth_catalog::find(
            static_cast<std::uint8_t>(definition.slotType), definition.authSchema);
        if (auth == nullptr) {
            lua_pushnil(state);
        } else if (key == "auth_type") {
            lua_pushlstring(state, auth->name.data(), auth->name.size());
        } else if (key == "auth_min_bits") {
            lua_pushinteger(state, auth->minimumBits);
        } else if (key == "auth_max_bits") {
            lua_pushinteger(state, auth->maximumBits);
        } else if (key == "auth_component_offset") {
            if (auth->hasContiguousMirror) {
                lua_pushinteger(state, auth->componentOffset);
            } else {
                lua_pushnil(state);
            }
        } else if (key == "auth_dynamic") {
            lua_pushboolean(state, auth->hasDynamicBody);
        } else {
            lua_pushboolean(state, auth->writable);
        }
    } else if (key == "flags") {
        lua_pushinteger(state, definition.flags);
    } else if (key == "set_object_filter") {
        lua_pushcfunction(state, &slot_set_object_filter);
    } else if (key == "set_volume_active") {
        lua_pushcfunction(state, &slot_set_volume_active);
    } else if (key == "set_squad_attachment") {
        lua_pushcfunction(state, &slot_set_squad_attachment);
    } else if (key == "set_mission_effect") {
        lua_pushcfunction(state, &slot_set_mission_effect);
    } else if (key == "watch_damage") {
        lua_pushcfunction(state, &slot_watch_damage);
    } else if (key == "set_object_active") {
        lua_pushcfunction(state, &slot_set_object_active);
    } else if (key == "set_channel") {
        lua_pushcfunction(state, &slot_set_channel);
    } else if (key == "transition") {
        lua_pushcfunction(state, &slot_transition);
    } else if (key == "set_occupancy_condition") {
        lua_pushcfunction(state, &slot_set_occupancy_condition);
    } else if (key == "generate_map") {
        lua_pushcfunction(state, &slot_generate_map);
    } else if (key == "set_directive") {
        lua_pushcfunction(state, &slot_set_directive);
    } else if (key == "clear_directives") {
        lua_pushcfunction(state, &slot_clear_directives);
    } else if (key == "set_darkness_zone") {
        lua_pushcfunction(state, &slot_set_darkness_zone);
    } else if (key == "set_music_section") {
        lua_pushcfunction(state, &slot_set_music_section);
    } else if (key == "set_interactable_object") {
        lua_pushcfunction(state, &slot_set_interactable_object);
    } else if (key == "set_ghost_link") {
        lua_pushcfunction(state, &slot_set_ghost_link);
    } else if (key == "ghost_link") {
        lua_pushcfunction(state, &slot_ghost_link);
    } else if (key == "set_public_event_state") {
        lua_pushcfunction(state, &slot_set_public_event_state);
    } else if (key == "assign_combat_objective") {
        lua_pushcfunction(state, &slot_assign_combat_objective);
    } else if (key == "play_actor_path") {
        lua_pushcfunction(state, &slot_play_actor_path);
    } else if (key == "play_actor_action") {
        lua_pushcfunction(state, &slot_play_actor_action);
    } else if (key == "retire_actor") {
        lua_pushcfunction(state, &slot_retire_actor);
    } else if (key == "bind_combatant_to_squad") {
        lua_pushcfunction(state, &slot_bind_combatant_to_squad);
    } else if (key == "run_atoms") {
        lua_pushcfunction(state, &slot_run_atoms);
    } else if (key == "fire_trigger") {
        lua_pushcfunction(state, &slot_fire_trigger);
    } else if (key == "disarm_trigger") {
        lua_pushcfunction(state, &slot_disarm_trigger);
    } else if (key == "play_sequence") {
        lua_pushcfunction(state,
                          exact_combatant_slot(definition) ? &slot_play_actor_sequence
                                                           : &slot_play_sequence);
    } else if (key == "sequences") {
        lua_pushcfunction(state, &slot_actor_sequences);
    } else if (key == "set_cinematic_active") {
        lua_pushcfunction(state, &slot_set_cinematic_active);
    } else if (key == "reset_objectives") {
        lua_pushcfunction(state, &slot_reset_objectives);
    } else if (key == "advance_task") {
        lua_pushcfunction(state, &slot_advance_task);
    } else if (key == "play_dialogue_cue") {
        lua_pushcfunction(state, &slot_play_dialogue_cue);
    } else if (key == "play_performance") {
        lua_pushcfunction(state, &slot_play_performance);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

void register_slot_metatables(lua_State* state) {
    register_metatable(state, kSlotMetatable, &slot_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
