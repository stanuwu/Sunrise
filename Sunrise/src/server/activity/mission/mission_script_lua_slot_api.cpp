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
#include "mission_script_lua_dispatch.h"
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
        frame.intentAllocationFailed = true;
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

namespace {

constexpr const api::MemberSpec& kVolumeActive = api::slot_member("set_volume_active");
constexpr const api::MemberSpec& kSquadAttachment = api::slot_member("set_squad_attachment");
constexpr const api::MemberSpec& kMissionEffect = api::slot_member("set_mission_effect");

/** Reads one required boolean argument; any other type is an error. */
[[nodiscard]] bool required_boolean(lua_State* state, const char* name) {
    lua_getfield(state, 2, name);
    if (!lua_isboolean(state, -1)) {
        luaL_error(state, "%s must be a boolean", name);
    }
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

/** Turns one authored type-60 volume of the same object on or off through its type-32 toggle. */
[[nodiscard]] int slot_set_volume_active(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kVolumeActive);
    const SlotHandle target = checked_argument<SlotHandle>(
        state, required_key(kVolumeActive, "volume", api::ValueKind::handle), kSlotMetatable);
    const bool active =
        required_boolean(state, required_key(kVolumeActive, "active", api::ValueKind::boolean));
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

/**
 * Selects or clears the squad a type-26 attachment follows. spawn_generation names the source
 * placement; a request for any other placement is refused as stale.
 */
[[nodiscard]] int slot_set_squad_attachment(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kSquadAttachment);
    const SlotHandle sourceHandle = checked_argument<SlotHandle>(
        state, required_key(kSquadAttachment, "source", api::ValueKind::handle), kSlotMetatable);
    const lua_Integer generation = checked_integer_argument(
        state, required_key(kSquadAttachment, "spawn_generation", api::ValueKind::integer));
    const bool active =
        required_boolean(state, required_key(kSquadAttachment, "active", api::ValueKind::boolean));
    SlotDefinition target{}, source{};
    if (!current_slot(state, *handle, target) || !current_slot(state, sourceHandle, source)) {
        return luaL_error(state, "attachment or squad source is stale or invalid");
    }
    if (target.slotType != scriptable_auth::kType26SlotType
        || target.authSchema != scriptable_auth::kType26Schema
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
    refuse_unknown_arguments(state, kMissionEffect);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != effect::kSlotType
        || slot.authSchema != effect::kSchema) {
        return luaL_error(state, "mission effect requires an authored type-26 hop-on");
    }
    const bool enabled =
        optional_boolean_argument(state, boolean_option(kMissionEffect, "enabled"));
    const char* const filterKey = optional_key(kMissionEffect, "filter", api::ValueKind::handle);
    scriptable_auth::Type2LaneClientRef filter{};
    if (enabled) {
        lua_getfield(state, 2, filterKey);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!present
            || !optional_slot_reference(
                state, filterKey, scriptable_auth::kType34SlotType, filter)) {
            return luaL_error(state, "mission effect requires a type-34 filter");
        }
    }
    const lua_Integer revision =
        optional_integer_argument(state, integer_option(kMissionEffect, "revision"));
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

/** Pushes one property of the slot's Auth catalog entry, or nil when it has none. */
template <typename Push> void push_auth(lua_State* state, const SlotDefinition& slot, Push push) {
    const auth_catalog::Type* const auth =
        auth_catalog::find(static_cast<std::uint8_t>(slot.slotType), slot.authSchema);
    if (auth == nullptr) {
        lua_pushnil(state);
    } else {
        push(*auth);
    }
}

/** Pushes one fixed-size text field of a slot row. */
template <typename Text> void push_text(lua_State* state, const Text& text) {
    lua_pushlstring(state, text.data(), text.size());
}

using SlotBinding = Binding<SlotDefinition>;

/** Slot members in table order. play_sequence takes the actor form on an exact combatant. */
constexpr std::array<SlotBinding, std::size(api::kSlotMembers)> kSlotBindings{{
    {"row",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.localRow); }},
    {"applied", &slot_applied},
    {"id",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { push_text(state, slot.id); }},
    {"name",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { push_text(state, slot.name); }},
    {"object_id",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { push_text(state, slot.objectId); }},
    {"object_tag",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.objectTag); }},
    {"registry_key",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         lua_pushinteger(state, slot.registryKey);
     }},
    {"slot_index",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.slotIndex); }},
    {"slot_type",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.slotType); }},
    {"component_class",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         lua_pushinteger(state, slot.componentClass);
     }},
    {"sense_schema",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         lua_pushinteger(state, slot.senseSchema);
     }},
    {"auth_schema",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.authSchema); }},
    {"sense_schema_id",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { push_text(state, slot.senseSchemaId); }},
    {"auth_schema_id",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { push_text(state, slot.authSchemaId); }},
    {"auth_type",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             lua_pushlstring(state, auth.name.data(), auth.name.size());
         });
     }},
    {"auth_min_bits",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             lua_pushinteger(state, auth.minimumBits);
         });
     }},
    {"auth_max_bits",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             lua_pushinteger(state, auth.maximumBits);
         });
     }},
    {"auth_component_offset",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             if (auth.hasContiguousMirror) {
                 lua_pushinteger(state, auth.componentOffset);
             } else {
                 lua_pushnil(state);
             }
         });
     }},
    {"auth_dynamic",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             lua_pushboolean(state, auth.hasDynamicBody);
         });
     }},
    {"auth_writable",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) {
         push_auth(state, slot, [state](const auth_catalog::Type& auth) {
             lua_pushboolean(state, auth.writable);
         });
     }},
    {"flags",
     nullptr,
     [](lua_State* state, const SlotDefinition& slot) { lua_pushinteger(state, slot.flags); }},
    {"set_object_filter", &slot_set_object_filter},
    {"set_volume_active", &slot_set_volume_active},
    {"set_squad_attachment", &slot_set_squad_attachment},
    {"set_mission_effect", &slot_set_mission_effect},
    {"watch_damage", &slot_watch_damage},
    {"set_object_active", &slot_set_object_active},
    {"set_channel", &slot_set_channel},
    {"transition", &slot_transition},
    {"set_occupancy_condition", &slot_set_occupancy_condition},
    {"generate_map", &slot_generate_map},
    {"set_directive", &slot_set_directive},
    {"clear_directives", &slot_clear_directives},
    {"set_darkness_zone", &slot_set_darkness_zone},
    {"set_music_section", &slot_set_music_section},
    {"set_interactable_object", &slot_set_interactable_object},
    {"set_ghost_link", &slot_set_ghost_link},
    {"ghost_link", &slot_ghost_link},
    {"set_public_event_state", &slot_set_public_event_state},
    {"assign_combat_objective", &slot_assign_combat_objective},
    {"play_actor_path", &slot_play_actor_path},
    {"play_actor_action", &slot_play_actor_action},
    {"retire_actor", &slot_retire_actor},
    {"bind_combatant_to_squad", &slot_bind_combatant_to_squad},
    {"run_atoms", &slot_run_atoms},
    {"fire_trigger", &slot_fire_trigger},
    {"disarm_trigger", &slot_disarm_trigger},
    {"play_sequence", &slot_play_actor_sequence, nullptr, &exact_combatant_slot},
    {"play_sequence", &slot_play_sequence},
    {"sequences", &slot_actor_sequences},
    {"set_cinematic_active", &slot_set_cinematic_active},
    {"reset_objectives", &slot_reset_objectives},
    {"advance_task", &slot_advance_task},
    {"play_dialogue_cue", &slot_play_dialogue_cue},
    {"play_performance", &slot_play_performance},
}};
static_assert(bound_to<SlotDefinition>(api::kSlotMembers, kSlotBindings));

} // namespace

/** Reads one Slot row member, its syntax methods, and its authorized actions. */
[[nodiscard]] int slot_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition)) {
        return luaL_error(state, "activity slot is stale");
    }
    return dispatch<SlotDefinition>(state, kSlotBindings, lua_string_view(state, 2), definition);
}

void register_slot_metatables(lua_State* state) {
    register_metatable(state, kSlotMetatable, &slot_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
