#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/auth_schema_catalog.h"
#include "../../../middleware/bap/activity_message/combatant_auth.h"
#include "../../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../../middleware/bap/activity_message/music_section_auth.h"
#include "../../../middleware/bap/activity_message/scene_events_auth.h"
#include "../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../middleware/bap/activity_message/squad_objective_auth.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "../../../state/activity_sdk/format.h"
#include "../../../state/activity_sdk/runtime.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace slot_transport = middleware::bap::activity_message::sensor_auth_update;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace auth_catalog = middleware::bap::activity_message::auth_schema_catalog;
namespace auth_fields = middleware::bap::activity_message::auth_fields;

namespace {
/** @return True when one live Slot row is an exact type-23 device. */
[[nodiscard]] bool exact_device_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kDeviceSlotType
           && definition.componentClass == format::kDeviceComponentClass
           && definition.senseSchema == format::kDeviceSenseSchema
           && definition.authSchema == format::kDeviceAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

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

/** @return True when one live Slot row is an exact type-4 authored object. */
[[nodiscard]] bool exact_object_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kObjectSlotType
           && definition.componentClass == format::kObjectComponentClass
           && definition.senseSchema == format::kObjectSenseSchema
           && definition.authSchema == format::kObjectAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-31 configured trigger. */
[[nodiscard]] bool exact_trigger_slot(const SlotDefinition& definition) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    return definition.slotType == auth::kType31SlotType
           && definition.authSchema == auth::kType31Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

[[nodiscard]] bool exact_sequence_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kSequenceSlotType
           && definition.componentClass == format::kSequenceComponentClass
           && definition.authSchema == format::kSequenceAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

[[nodiscard]] bool exact_cinematic_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kCinematicSlotType
           && definition.componentClass == format::kCinematicComponentClass
           && definition.authSchema == format::kCinematicAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is the exact type-3 objective reset control. */
[[nodiscard]] bool exact_objective_reset_slot(const SlotDefinition& definition) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    return definition.slotType == auth::kType3SlotType
           && definition.authSchema == auth::kType3Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

[[nodiscard]] bool exact_task_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kTaskSlotType
           && definition.componentClass == format::kTaskComponentClass
           && definition.authSchema == format::kTaskAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

[[nodiscard]] bool exact_dialogue_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kDialogueSlotType
           && definition.componentClass == format::kDialogueComponentClass
           && definition.authSchema == format::kDialogueAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0
           && (definition.flags & format::kSlotDialogueCuesExact) != 0;
}

/** @return True when one live Slot row is an exact type-30 occupancy condition. */
[[nodiscard]] bool exact_occupancy_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kOccupancySlotType
           && definition.componentClass == format::kOccupancyComponentClass
           && definition.senseSchema == format::kOccupancySenseSchema
           && definition.authSchema == format::kOccupancyAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is the exact type-68 HUD directive state. */
[[nodiscard]] bool exact_directive_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType68SlotType
           && definition.authSchema == scriptable_auth::kType68Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is the exact type-70 engagement observer. */
[[nodiscard]] bool exact_engagement_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType70SlotType
           && definition.authSchema == scriptable_auth::kType70Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is the exact type-71 public-event sensor. */
[[nodiscard]] bool exact_public_event_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType71SlotType
           && definition.authSchema == scriptable_auth::kType71Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-42 performance sensor. */
[[nodiscard]] bool exact_performance_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType42SlotType
           && definition.componentClass == scriptable_auth::kType42ComponentClass
           && definition.authSchema == scriptable_auth::kType42Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** The occupancy Auth body is a fixed 87 bits: a 55-bit client reference then one int32. */
constexpr std::size_t kOccupancyAuthBitCount = 87;
constexpr std::size_t kOccupancyAuthByteCount = 11;
} // namespace

/** @return True when one live Slot row is an exact type-2 combatant. */
[[nodiscard]] bool exact_combatant_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType2SlotType
           && definition.componentClass == scriptable_auth::kType2ComponentClass
           && definition.senseSchema == scriptable_auth::kType2SenseSchema
           && definition.authSchema == scriptable_auth::kType2Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** Stages one authored native Auth body on the guarded message-5 route. */
[[nodiscard]] int queue_slot_auth(lua_State* state,
                                  const SlotDefinition& slot,
                                  std::uint32_t schema,
                                  std::size_t bitCount,
                                  std::span<const std::byte> body) {
    Impl* const impl = impl_from_state(state);
    std::array<std::byte, 32> sdkBuildSha256{};
    if (!decode_sdk_build_sha256(std::string_view(impl->identity.sdkBuildId.data()),
                                 sdkBuildSha256)) {
        return luaL_error(state, "loaded SDK generation identity is invalid");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::applySlotAuth;
    intent.sdkBuildSha256 = sdkBuildSha256;
    intent.firstRow = slot.nativeRow;
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

/** Sets the authored object filter and caller value carried by one type-30 condition. */
[[nodiscard]] int slot_set_occupancy_condition(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 2> kDeclared{"filter", "value"};
    refuse_unknown_arguments(state, kDeclared);
    const SlotHandle reference = checked_argument<SlotHandle>(state, "filter", kSlotMetatable);
    const lua_Integer value = checked_integer_argument(state, "value");
    SlotDefinition slot{};
    SlotDefinition playerSet{};
    if (!current_slot(state, *handle, slot) || !current_slot(state, reference, playerSet)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_occupancy_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-30 occupancy condition");
    }
    // The Auth field is a full-range int32 and lua_Integer is wider, so the lane is still checked.
    if (value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "value must be a 32-bit signed integer");
    }
    std::array<std::byte, kOccupancyAuthByteCount> body{};
    middleware::encoding::bits::Writer writer(body);
    const std::uint32_t encodedValue =
        std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value)) + 0x80000000U;
    if (!writer.write(playerSet.registryKey, 32)
        || !writer.write(static_cast<std::uint32_t>(playerSet.slotType) + 1U, 7)
        || !writer.write(static_cast<std::uint32_t>(playerSet.slotIndex) + 32768U, 16)
        || !writer.write(encodedValue, 32) || writer.bit_count() != kOccupancyAuthBitCount) {
        return luaL_error(state, "occupancy condition native encoder failed");
    }
    return queue_slot_auth(state, slot, format::kOccupancyAuthSchema, kOccupancyAuthBitCount, body);
}

/** Reads one integer field from a generated directive declaration. */
[[nodiscard]] lua_Integer directive_integer(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    const lua_Integer value = luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    return value;
}

/** Shows one generated directive through the exact type-68 Auth schema. */
[[nodiscard]] int slot_set_directive(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 4> kDeclared{
        "directive", "state", "navpoint", "audience"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_directive_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-68 directive state");
    }
    lua_getfield(state, 2, "directive");
    luaL_checktype(state, -1, LUA_TTABLE);
    const lua_Integer slotRow = directive_integer(state, -1, "slot_row");
    const lua_Integer nameHash = directive_integer(state, -1, "name_hash");
    const lua_Integer element = directive_integer(state, -1, "element");
    lua_pop(state, 1);
    const lua_Integer directiveState = optional_integer_argument(state, "state", 0);
    if (slotRow < 0 || slotRow > (std::numeric_limits<std::uint32_t>::max)() || nameHash < 0
        || nameHash > (std::numeric_limits<std::uint32_t>::max)() || element < 0
        || element > (std::numeric_limits<std::int32_t>::max)() || directiveState < 0
        || directiveState > 2) {
        return luaL_error(state, "directive declaration is outside its native field width");
    }
    Impl* const impl = impl_from_state(state);
    DirectiveElementDefinition resolved{};
    if (impl == nullptr || impl->definitions.resolveDirectiveElement == nullptr
        || !impl->definitions.resolveDirectiveElement(impl->definitions.context,
                                                      static_cast<std::uint32_t>(slotRow),
                                                      static_cast<std::uint32_t>(nameHash),
                                                      static_cast<std::int32_t>(element),
                                                      resolved)
        || resolved.slotRow != slot.nativeRow) {
        return luaL_error(state, "directive does not belong to this slot");
    }
    scriptable_auth::Type68Preset preset{.nameHash = resolved.nameHash,
                                         .elementIndex = resolved.elementIndex,
                                         .state = static_cast<std::int8_t>(directiveState),
                                         .visible = true};
    if (!optional_slot_reference(
            state, "audience", scriptable_auth::kType70SlotType, preset.audience)) {
        return luaL_error(state,
                          "directive audience requires an authored type-70 engagement sensor");
    }
    if (!optional_slot_reference(
            state, "navpoint", scriptable_auth::kType47SlotType, preset.navpoint)) {
        return luaL_error(state, "directive navpoint requires a current authored type-47 slot");
    }
    std::array<std::byte, scriptable_auth::kType68ByteCount> body{};
    std::size_t written = 0;
    if (!scriptable_auth::encode_type68(preset, body, written) || written != body.size()) {
        return luaL_error(state, "directive native encoder failed");
    }
    return queue_slot_auth(
        state, slot, scriptable_auth::kType68Schema, scriptable_auth::kType68BitCount, body);
}

/** Hides the active directive without naming an authored element. */
[[nodiscard]] int slot_clear_directives(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_directive_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-68 directive state");
    }
    scriptable_auth::Type68Preset preset{};
    preset.visible = false;
    std::array<std::byte, scriptable_auth::kType68ByteCount> body{};
    std::size_t written = 0;
    if (!scriptable_auth::encode_type68(preset, body, written) || written != body.size()) {
        return luaL_error(state, "directive native encoder failed");
    }
    return queue_slot_auth(
        state, slot, scriptable_auth::kType68Schema, scriptable_auth::kType68BitCount, body);
}

/** Restores one encounter engagement sensor with script-declared native initial values. */
[[nodiscard]] int slot_set_engagement_state(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_engagement_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-70 engagement state");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 2> kDeclared{"flags", "revision"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer flags = optional_integer_argument(state, "flags", 1);
    const lua_Integer revision = optional_integer_argument(state, "revision", 1);
    if (flags < 0 || flags > 0x1F || revision < (std::numeric_limits<std::int16_t>::min)()
        || revision > (std::numeric_limits<std::int16_t>::max)()) {
        return luaL_error(state, "engagement state is outside its native field width");
    }
    const scriptable_auth::Type70Preset preset{
        .flags = static_cast<std::uint8_t>(flags),
        .revision = static_cast<std::int16_t>(revision),
    };
    std::array<std::byte, scriptable_auth::kType70ByteCount> body{};
    std::size_t written = 0;
    if (!scriptable_auth::encode_type70(preset, body, written) || written != body.size()) {
        return luaL_error(state, "engagement native encoder failed");
    }
    return queue_slot_auth(
        state, slot, scriptable_auth::kType70Schema, scriptable_auth::kType70BitCount, body);
}

/** Enables the native darkness restriction; roster assembly supplies its matching bubble. */
[[nodiscard]] int slot_set_darkness_zone(lua_State* state) {
    namespace darkness = middleware::bap::activity_message::darkness_zone;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"enabled", "wipe_seconds"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != darkness::kSlotType
        || slot.componentClass != darkness::kComponentClass || slot.authSchema != darkness::kSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return luaL_error(state, "darkness zone requires the exact hard-wipe globals sensor");
    }
    const lua_Integer wipe = optional_integer_argument(state, "wipe_seconds", darkness::kNoWipe);
    std::array<std::byte, darkness::kBytes> body{};
    if (wipe < darkness::kNoWipe || wipe > darkness::kMaximumWipeSeconds
        || !darkness::encode(
            optional_boolean_argument(state, "enabled", false), body, static_cast<int>(wipe))) {
        return luaL_error(state, "darkness zone encoder failed");
    }
    return queue_slot_auth(state, slot, darkness::kSchema, darkness::kBits, body);
}

/** Volumes one filter may test; leaves room for the players, target and inside predicates. */
constexpr std::size_t kMaximumFilterVolumes = 5;
/** Type-34 predicate modes: 0 tests the flag or reference as given, 1 tests inside a volume. */
constexpr std::int8_t kFilterModeDirect = 0;
constexpr std::int8_t kFilterModeInside = 1;

/** Native typed object filters: players, one object, and volume intersection. */
[[nodiscard]] int slot_set_object_filter(lua_State* state) {
    namespace auth = scriptable_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 4> kDeclared{
        "players", "target", "inside", "inside_any"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != auth::kType34SlotType
        || slot.authSchema != auth::kType34Schema) {
        return luaL_error(state, "object filter requires an authored type-34 sensor");
    }
    auth::Type34Body body{};
    lua_getfield(state, 2, "inside_any");
    const bool volumes = !lua_isnil(state, -1);
    if (volumes) {
        luaL_checktype(state, -1, LUA_TTABLE);
        const std::size_t count = lua_rawlen(state, -1);
        if (count == 0 || count > kMaximumFilterVolumes) {
            return luaL_error(state, "inside_any volume count is outside the filter capacity");
        }
        for (std::size_t index = 1; index <= count; ++index) {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
            const auto* const volumeHandle =
                static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
            SlotDefinition volume{};
            if (!current_slot(state, *volumeHandle, volume)
                || volume.slotType != auth::kType60SlotType) {
                return luaL_error(state, "inside_any requires authored type-60 volumes");
            }
            body.predicates[body.count++] =
                auth::Type34ModeFlagSlotRef{kFilterModeDirect,
                                            true,
                                            {volume.registryKey,
                                             static_cast<std::int8_t>(auth::kType60SlotType),
                                             static_cast<std::int16_t>(volume.slotIndex)}};
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    if (optional_boolean_argument(state, "players", false)) {
        body.predicates[body.count++] =
            auth::Type34ModeOnlyB{static_cast<std::int8_t>(volumes ? 1 : 0)};
    }
    auth::Type2LaneClientRef target{};
    if (!optional_slot_reference(state, "target", auth::kType4SlotType, target)) {
        return luaL_error(state, "filter target must be an authored type-4 object");
    }
    if (target.slotIndex >= 0) {
        body.predicates[body.count++] = auth::Type34ModeSlotRefC{kFilterModeDirect, target};
    }
    auth::Type2LaneClientRef inside{};
    if (!optional_slot_reference(state, "inside", auth::kType60SlotType, inside)) {
        return luaL_error(state, "filter inside must be an authored type-60 volume");
    }
    if (inside.slotIndex >= 0) {
        body.predicates[body.count++] =
            auth::Type34ModeFlagSlotRef{kFilterModeInside, false, inside};
    }
    std::array<std::byte, auth::kType34MaximumByteCount> bytes{};
    std::size_t written = 0;
    std::size_t bits = 0;
    if (!auth::encode_type34(body, bytes, written, bits)) {
        return luaL_error(state, "object filter encoder failed");
    }
    return queue_slot_auth(state, slot, auth::kType34Schema, bits, std::span(bytes).first(written));
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

/** Binds an authored damage monitor to one exact object; a new revision re-binds it. */
[[nodiscard]] int slot_watch_damage(lua_State* state) {
    namespace damage = middleware::bap::activity_message::damage_monitor;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"target", "revision"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != damage::kSlotType
        || slot.authSchema != damage::kAuthSchema) {
        return luaL_error(state, "damage watch requires an authored type-20 monitor");
    }
    lua_getfield(state, 2, "target");
    const auto* const targetHandle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
    SlotDefinition target{};
    const bool exact = current_slot(state, *targetHandle, target) && exact_object_slot(target);
    lua_pop(state, 1);
    if (!exact) {
        return luaL_error(state, "damage target must be an authored type-4 object");
    }
    const lua_Integer revision = optional_integer_argument(state, "revision", 1);
    if (!valid_counter(revision)) {
        return luaL_error(state, "damage revision must be positive");
    }
    std::array<std::byte, damage::kBytes> body{};
    std::size_t written = 0;
    if (!damage::encode(target.registryKey,
                        static_cast<std::uint16_t>(target.slotIndex),
                        static_cast<std::int32_t>(revision),
                        body,
                        written)) {
        return luaL_error(state, "damage monitor encoder failed");
    }
    return queue_slot_auth(state, slot, damage::kAuthSchema, damage::kBits, body);
}

/** Selects one authored section in a native music sensor's selection mask. */
[[nodiscard]] int slot_set_music_section(lua_State* state) {
    namespace music = middleware::bap::activity_message::music_section;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"section", "enabled"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    const lua_Integer section = checked_integer_argument(state, "section");
    if (!current_slot(state, *handle, slot) || slot.slotType != music::kSlotType
        || slot.componentClass != music::kComponentClass || slot.authSchema != music::kSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0 || section < 0
        || section >= static_cast<lua_Integer>(music::kSectionCount)) {
        return luaL_error(state, "music requires an exact type-11 sensor and a section index");
    }
    std::array<std::byte, music::kBytes> body{};
    std::size_t written = 0;
    if (!music::encode(static_cast<std::uint8_t>(section),
                       optional_boolean_argument(state, "enabled", true),
                       body,
                       written)) {
        return luaL_error(state, "music section encoder failed");
    }
    return queue_slot_auth(state, slot, music::kSchema, music::kBits, body);
}

/** Spawns authored entry zero and subscribes to accepted native player use. */
[[nodiscard]] int slot_set_interactable_object(lua_State* state) {
    namespace object = middleware::bap::activity_message::interactable_object;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{
        "generation", "track_owner", "active"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_object_slot(slot)) {
        return luaL_error(state, "interaction requires an exact authored object");
    }
    const lua_Integer generation = optional_integer_argument(state, "generation", 1);
    if (!valid_counter(generation)) {
        return luaL_error(state, "object generation must be a positive int32");
    }
    const bool trackOwner = optional_boolean_argument(state, "track_owner", false);
    std::array<std::byte, object::kOwnerBytes> body{};
    std::size_t written = 0;
    if (!object::encode(static_cast<std::int32_t>(generation),
                        body,
                        written,
                        trackOwner,
                        optional_boolean_argument(state, "active", true))) {
        return luaL_error(state, "interactable object encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           object::kSchema,
                           trackOwner ? object::kOwnerBits : object::kBits,
                           std::span(body).first(written));
}

/** Enables the authored Ghost interaction without replacing its action hash. */
[[nodiscard]] int slot_set_ghost_link(lua_State* state) {
    namespace ghost = middleware::bap::activity_message::ghost_link;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"generation", "enabled"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != ghost::kSlotType
        || slot.componentClass != ghost::kComponentClass || slot.authSchema != ghost::kAuthSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return luaL_error(state, "activity slot is not an exact Ghost-link sensor");
    }
    const lua_Integer generation = checked_integer_argument(state, "generation");
    if (!valid_counter(generation)) {
        return luaL_error(state, "Ghost-link generation must be a positive int32");
    }
    const bool enabled = optional_boolean_argument(state, "enabled", true);
    std::array<std::byte, ghost::kByteCount> body{};
    std::size_t written = 0;
    if (!ghost::encode(static_cast<std::int32_t>(generation), enabled, body, written)) {
        return luaL_error(state, "Ghost-link native encoder failed");
    }
    return queue_slot_auth(state, slot, ghost::kAuthSchema, ghost::kBitCount, body);
}

/** Names the player, the event area and the leave timeout one public-event sensor watches. */
[[nodiscard]] int slot_set_public_event_state(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_public_event_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-71 public-event sensor");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 4> kDeclared{
        "state", "player", "area", "leave_seconds"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer eventState = optional_integer_argument(state, "state", 0);
    if (eventState < (std::numeric_limits<std::int32_t>::min)()
        || eventState > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "state must be a 32-bit signed integer");
    }
    // Absent, the sensor watches this link's own player. Present, it is a decimal string, the
    // form every 64-bit key crosses into Lua in.
    std::uint64_t player = impl_from_state(state)->identity.playerKey;
    if (push_argument(state, "player") != LUA_TNIL) {
        lua_pop(state, 1);
        const std::string_view playerText = borrowed_string_argument(state, "player");
        const auto parsed =
            std::from_chars(playerText.data(), playerText.data() + playerText.size(), player);
        if (parsed.ec != std::errc{} || parsed.ptr != playerText.data() + playerText.size()) {
            return luaL_error(state, "player must be a decimal player key string");
        }
        lua_pop(state, 1);
    } else {
        lua_pop(state, 1);
    }
    if (player == 0) {
        return luaL_error(state, "no player key is known for this activity link");
    }
    const SlotHandle areaHandle = checked_argument<SlotHandle>(state, "area", kSlotMetatable);
    const lua_Number seconds = checked_number_argument(state, "leave_seconds");
    SlotDefinition area{};
    if (!current_slot(state, areaHandle, area)) {
        return luaL_error(state, "area slot is stale or invalid");
    }
    if (!std::isfinite(seconds) || seconds < 0.0
        || seconds > static_cast<lua_Number>((std::numeric_limits<float>::max)())) {
        return luaL_error(state, "leave_seconds must be a finite non-negative number");
    }
    const scriptable_auth::Type71Body body{
        .state = static_cast<std::int32_t>(eventState),
        .playerIdentity = player,
        .areaRegistryKey = area.registryKey,
        .areaSlotType = static_cast<std::uint8_t>(area.slotType),
        .areaSlotIndex = static_cast<std::uint16_t>(area.slotIndex),
        .leaveSeconds = static_cast<float>(seconds),
    };
    std::array<std::byte, scriptable_auth::kType71ByteCount> bytes{};
    std::size_t written = 0;
    if (!scriptable_auth::encode_type71(body, bytes, written) || written != bytes.size()) {
        return luaL_error(state, "public-event native encoder failed");
    }
    return queue_slot_auth(
        state, slot, scriptable_auth::kType71Schema, scriptable_auth::kType71BitCount, bytes);
}

/** @return True when one live Slot row is an exact squad in the given registry. */
[[nodiscard]] bool exact_squad_of(const SlotDefinition& squad,
                                  const SlotDefinition& owner) noexcept {
    return squad.slotType == format::kSquadSlotType
           && squad.componentClass == format::kSquadComponentClass
           && (squad.flags & format::kSlotSchemaJoinExact) != 0
           && squad.registryKey == owner.registryKey && squad.objectTag == owner.objectTag
           && squad.slotIndex <= auth_fields::kMaximumClientRefIndex;
}

/** Assigns a squad to a native combat objective and its selected task group. */
[[nodiscard]] int slot_assign_combat_objective(lua_State* state) {
    namespace objective = middleware::bap::activity_message::squad_objective;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 5> kDeclared{
        "objective", "revision", "task_group", "reserved", "refresh_player_awareness"};
    refuse_unknown_arguments(state, kDeclared);
    const auto reference = checked_argument<SlotHandle>(state, "objective", kSlotMetatable);
    const lua_Integer revision = checked_integer_argument(state, "revision");
    const lua_Integer group = checked_integer_argument(state, "task_group");
    const bool reserved = optional_boolean_argument(state, "reserved", false);
    const bool refresh = optional_boolean_argument(state, "refresh_player_awareness", false);
    SlotDefinition squad{};
    SlotDefinition target{};
    if (!current_slot(state, *handle, squad) || squad.slotType != format::kSquadSlotType
        || squad.componentClass != format::kSquadComponentClass
        || squad.authSchema != objective::kSchema
        || (squad.flags & format::kSlotSchemaJoinExact) == 0
        || !current_slot(state, reference, target) || !exact_objective_reset_slot(target)
        || target.componentClass != format::kObjectiveComponentClass
        || squad.registryKey != target.registryKey) {
        return luaL_error(
            state, "combat objective requires exact squad/objective slots in the same registry");
    }
    if (!valid_counter(revision) || group < objective::kNoTaskGroup
        || group >= objective::kTaskGroupCount
        || target.slotIndex > auth_fields::kMaximumClientRefIndex) {
        return luaL_error(state,
                          "combat objective revision, group or index is outside its native range");
    }
    const objective::Request request{target.registryKey,
                                     static_cast<std::uint32_t>(revision),
                                     static_cast<std::uint16_t>(target.slotIndex),
                                     static_cast<std::int32_t>(group),
                                     reserved,
                                     refresh};
    std::array<std::byte, objective::kMaximumBytes> body{};
    const auto encoded = std::span(body).first(objective::byte_count(request));
    if (!objective::encode(request, encoded)) {
        return luaL_error(state, "combat objective encoder failed");
    }
    return queue_slot_auth(
        state, squad, objective::kSchema, objective::bit_count(request), encoded);
}

/** Creates a named actor and starts one package-authored movement path. */
[[nodiscard]] int slot_play_actor_path(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{"generation", "revision", "path"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer generation = checked_integer_argument(state, "generation");
    const lua_Integer revision = checked_integer_argument(state, "revision");
    const auto reference = checked_argument<SlotHandle>(state, "path", kSlotMetatable);
    SlotDefinition actor{};
    SlotDefinition path{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !current_slot(state, reference, path) || path.slotType != combatant::kPathSlotType
        || path.componentClass != combatant::kPathComponentClass
        || actor.objectTag != path.objectTag || actor.registryKey != path.registryKey) {
        return luaL_error(state,
                          "actor path requires an exact member and same-registry type-58 path");
    }
    if (!valid_counter(generation) || !valid_counter(revision)
        || path.slotIndex > auth_fields::kMaximumClientRefIndex) {
        return luaL_error(state,
                          "actor path generation, revision or index is outside its native range");
    }
    std::array<std::byte, combatant::kPathBytes> body{};
    if (!combatant::encode_path({static_cast<std::uint32_t>(generation),
                                 static_cast<std::uint32_t>(revision),
                                 path.registryKey,
                                 static_cast<std::uint16_t>(path.slotIndex)},
                                body)) {
        return luaL_error(state, "actor path encoder failed");
    }
    return queue_slot_auth(state, actor, combatant::kSchema, combatant::kPathBits, body);
}

/** Runs an authored native custom action without recreating its actor. */
[[nodiscard]] int slot_play_actor_action(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    constexpr lua_Integer kMaximumHash = (std::numeric_limits<std::uint32_t>::max)();
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 4> kDeclared{
        "generation", "revision", "group", "action"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer generation = checked_integer_argument(state, "generation");
    const lua_Integer revision = checked_integer_argument(state, "revision");
    const lua_Integer group = checked_integer_argument(state, "group");
    const lua_Integer action = checked_integer_argument(state, "action");
    SlotDefinition actor{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !valid_counter(generation) || !valid_counter(revision) || group < 0
        || group > kMaximumHash || action <= 0 || action > kMaximumHash) {
        return luaL_error(state,
                          "actor action requires an exact member and valid native identities");
    }
    std::array<std::byte, combatant::kActionBytes> body{};
    if (!combatant::encode_action({static_cast<std::uint32_t>(generation),
                                   static_cast<std::uint32_t>(revision),
                                   static_cast<std::uint32_t>(group),
                                   static_cast<std::uint32_t>(action)},
                                  body)) {
        return luaL_error(state, "actor action encoder failed");
    }
    return queue_slot_auth(state, actor, combatant::kSchema, combatant::kActionBits, body);
}

/**
 * Encodes and queues one delivery manifest for the named actor.
 * @param squads Reserved squads in the actor's registry.
 */
[[nodiscard]] int queue_delivery(
    lua_State* state,
    const SlotDefinition& actor,
    lua_Integer generation,
    lua_Integer revision,
    std::span<const middleware::bap::activity_message::combatant_auth::SquadReference> squads) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    std::array<std::byte, combatant::kDeliveryMaximumBytes> body{};
    std::size_t written = 0;
    std::size_t bits = 0;
    if (!combatant::encode_delivery(static_cast<std::uint32_t>(generation),
                                    static_cast<std::uint32_t>(revision),
                                    squads,
                                    body,
                                    written,
                                    bits)) {
        return luaL_error(state, "invalid delivery manifest");
    }
    return queue_slot_auth(state, actor, combatant::kSchema, bits, std::span(body).first(written));
}

/** Gives one reserved squad to the named actor's native passenger-delivery component. */
[[nodiscard]] int slot_deliver_squad(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{"generation", "revision", "squad"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer generation = checked_integer_argument(state, "generation");
    const lua_Integer revision = checked_integer_argument(state, "revision");
    const auto reference = checked_argument<SlotHandle>(state, "squad", kSlotMetatable);
    SlotDefinition actor{};
    SlotDefinition squad{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !current_slot(state, reference, squad) || !exact_squad_of(squad, actor)) {
        return luaL_error(state, "delivery requires an exact member and same-registry squad");
    }
    if (!valid_counter(generation) || !valid_counter(revision)) {
        return luaL_error(state, "delivery generation or revision is outside its native range");
    }
    const std::array<combatant::SquadReference, 1> squads{
        {{squad.registryKey, static_cast<std::uint16_t>(squad.slotIndex)}}};
    return queue_delivery(state, actor, generation, revision, squads);
}

/** Sends one manifest so several reserved squads share the same ship and unload together. */
[[nodiscard]] int slot_deliver_squads(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{"generation", "revision", "squads"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer generation = checked_integer_argument(state, "generation");
    const lua_Integer revision = checked_integer_argument(state, "revision");
    SlotDefinition actor{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !valid_counter(generation) || !valid_counter(revision)) {
        return luaL_error(state,
                          "delivery requires an exact actor and positive generation/revision");
    }
    lua_getfield(state, 2, "squads");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int list = lua_gettop(state);
    const std::size_t count = lua_rawlen(state, list);
    if (count == 0 || count > combatant::kMaximumManifestSquads) {
        return luaL_error(state, "delivery manifest squad count is outside its native range");
    }
    std::array<combatant::SquadReference, combatant::kMaximumManifestSquads> squads{};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(index + 1));
        const auto* const squadHandle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition squad{};
        if (!current_slot(state, *squadHandle, squad) || !exact_squad_of(squad, actor)) {
            return luaL_error(state, "delivery squads must be exact and in the actor's registry");
        }
        squads[index] = {squad.registryKey, static_cast<std::uint16_t>(squad.slotIndex)};
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return queue_delivery(state, actor, generation, revision, std::span(squads).first(count));
}

/** Retires the named actor on a new generation, which also clears retained Auth on reload. */
[[nodiscard]] int slot_retire_actor(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 1> kDeclared{"generation"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer generation = checked_integer_argument(state, "generation");
    SlotDefinition actor{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !valid_counter(generation)) {
        return luaL_error(state,
                          "actor retirement requires an exact member and positive generation");
    }
    std::array<std::byte, combatant::kRetireBytes> body{};
    if (!combatant::encode_retire(static_cast<std::uint32_t>(generation), body)) {
        return luaL_error(state, "actor retirement encoder failed");
    }
    return queue_slot_auth(state, actor, combatant::kSchema, combatant::kRetireBits, body);
}

/**
 * Arms one exact combatant for its scene's squad member spawn.
 * @param state Lua call holding the slot and empty argument table.
 * @return One request handle after the intent is retained.
 */
[[nodiscard]] int slot_bind_combatant_to_squad(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_combatant_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-2 combatant");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::bindCombatantToSquad;
    intent.firstRow = definition.nativeRow;
    return queue_intent(state, frame, intent);
}

/**
 * Reads the optional `with` list: more object slots that answer on this one's revision.
 * @return False with the Lua error already raised.
 */
[[nodiscard]] bool parse_object_burst(lua_State* state, Intent& intent) {
    lua_getfield(state, 2, "with");
    if (lua_isnoneornil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        static_cast<void>(luaL_argerror(state, 2, "object burst list is not a table"));
        return false;
    }
    const int list = lua_gettop(state);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, list));
    if (count < 0
        || static_cast<std::size_t>(count)
               > ::sunrise::state::activity::mission::kIntentBurstCapacity) {
        lua_pop(state, 1);
        static_cast<void>(luaL_argerror(state, 2, "object burst list is too long"));
        return false;
    }
    for (lua_Integer entry = 1; entry <= count; ++entry) {
        lua_rawgeti(state, list, entry);
        SlotDefinition member{};
        const bool resolved =
            resolve_slot(state, lua_gettop(state), member) && exact_object_slot(member);
        lua_pop(state, 1);
        if (!resolved) {
            lua_pop(state, 1);
            static_cast<void>(
                luaL_argerror(state, 2, "object burst names a slot that is not an exact type-4"));
            return false;
        }
        intent.burstRows[static_cast<std::size_t>(entry - 1)] = member.nativeRow;
    }
    lua_pop(state, 1);
    intent.burstRowCount = static_cast<std::uint8_t>(count);
    return true;
}

/** Instantiates or removes the package-owned entry one type-4 slot selects. */
[[nodiscard]] int slot_set_object_active(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_object_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-4 authored object");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 2> kDeclared{"active", "with"};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setObjectActive;
    intent.firstRow = definition.nativeRow;
    intent.entryIndex = 0;
    intent.active = optional_boolean_argument(state, "active", true);
    if (!parse_object_burst(state, intent)) {
        return 0;
    }
    return queue_intent(state, frame, intent);
}

/** Sets one verified type-23 channel through the same guarded route. */
[[nodiscard]] int slot_set_channel(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_device_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-23 device");
    }
    // Both parameters carry their own bound, so neither the lane nor the range is tested here.
    static constexpr std::array<std::string_view, 3> kDeclared{"channel", "value", "snap"};
    refuse_unknown_arguments(state, kDeclared);
    const DeviceChannelHandle channel =
        checked_argument<DeviceChannelHandle>(state, "channel", kDeviceChannelMetatable);
    const UnitScalarHandle value =
        checked_argument<UnitScalarHandle>(state, "value", kUnitScalarMetatable);
    const bool snap = optional_boolean_argument(state, "snap", false);

    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setDeviceChannel;
    intent.firstRow = definition.nativeRow;
    intent.deviceValue = value.value;
    intent.deviceChannel = channel.channel;
    intent.deviceSnap = snap;
    return queue_intent(state, frame, intent);
}
/** Applies one named device transition through the same guarded channel route. */
[[nodiscard]] int slot_transition(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_device_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-23 device");
    }
    // The handle is a row of the closed vocabulary, so no word is matched here.
    static constexpr std::array<std::string_view, 2> kDeclared{"transition", "snap"};
    refuse_unknown_arguments(state, kDeclared);
    const DeviceTransitionHandle requested =
        checked_argument<DeviceTransitionHandle>(state, "transition", kDeviceTransitionMetatable);
    const bool snap = optional_boolean_argument(state, "snap", false);
    const DeviceTransition& transition = kDeviceTransitions[requested.row];
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setDeviceChannel;
    intent.firstRow = definition.nativeRow;
    intent.deviceValue = transition.value;
    intent.deviceChannel = static_cast<std::uint8_t>(transition.channel);
    intent.deviceSnap = snap;
    return queue_intent(state, frame, intent);
}
/**
 * Fires one type-31 configured trigger. One authored typed reference must be live and eligible.
 * The pulse carries no caller value. `enabled` is fixed true and the auxiliary stays zero, and
 * the Host mints the generation from the target's own guard so a replay cannot reorder.
 */
[[nodiscard]] int slot_fire_trigger(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_trigger_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-31 trigger");
    }
    // The pulse has no parameters, so an unsafe call cannot be spelled.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::fireTrigger;
    intent.firstRow = definition.nativeRow;
    return queue_intent(state, frame, intent);
}

/** Publishes one scene generation and its cumulative authored event keys. */
[[nodiscard]] int slot_set_scene_events(lua_State* state) {
    namespace scene = middleware::bap::activity_message::scene_events;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"generation", "events"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != scene::kSlotType
        || slot.componentClass != scene::kComponentClass || slot.authSchema != scene::kSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return luaL_error(state, "scene events require an exact type-43 scene");
    }
    const lua_Integer generation = checked_integer_argument(state, "generation");
    if (!valid_counter(generation)) {
        return luaL_error(state, "scene generation must be a positive int32");
    }
    lua_getfield(state, 2, "events");
    luaL_checktype(state, -1, LUA_TTABLE);
    const std::size_t count = lua_rawlen(state, -1);
    if (count > scene::kMaximumEvents) {
        return luaL_error(state, "scene event list exceeds the manifest capacity");
    }
    std::array<std::uint32_t, scene::kMaximumEvents> events{};
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        const lua_Integer event = luaL_checkinteger(state, -1);
        lua_pop(state, 1);
        if (event <= 0 || event >= static_cast<lua_Integer>(scene::kInvalidEventKey)) {
            return luaL_error(state, "invalid scene event key");
        }
        events[index] = static_cast<std::uint32_t>(event);
    }
    lua_pop(state, 1);
    std::array<std::byte, scene::kMaximumBytes> body{};
    std::size_t bytes = 0;
    std::size_t bits = 0;
    if (!scene::encode(static_cast<std::int32_t>(generation),
                       std::span(events).first(count),
                       body,
                       bytes,
                       bits)) {
        return luaL_error(state, "scene event keys must be unique");
    }
    return queue_slot_auth(state, slot, scene::kSchema, bits, std::span(body).first(bytes));
}

/** Lua `play_sequence` on a slot. Errors unless the slot is an exact type-5 sequence. */
[[nodiscard]] int slot_play_sequence(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_sequence_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-5 authored sequence");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::playSequence;
    intent.firstRow = definition.nativeRow;
    return queue_intent(state, frame, intent);
}

/** Lua `set_cinematic_active` on a slot. Errors unless the slot is an exact cinematic. */
[[nodiscard]] int slot_set_cinematic_active(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_cinematic_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-6 authored cinematic");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 1> kDeclared{"active"};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setCinematicActive;
    intent.firstRow = definition.nativeRow;
    intent.active = optional_boolean_argument(state, "active", true);
    return queue_intent(state, frame, intent);
}

/** Queues the parameter-free reset of every objective lane owned by one type-3 slot. */
[[nodiscard]] int slot_reset_objectives(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_objective_reset_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-3 objective reset");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::resetObjectives;
    intent.firstRow = definition.nativeRow;
    return queue_intent(state, frame, intent);
}

/** Advances the exact objective bit authored by one type-38 task slot. */
[[nodiscard]] int slot_advance_task(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_task_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-38 authored task");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::advanceTask;
    intent.firstRow = definition.nativeRow;
    return queue_intent(state, frame, intent);
}

/**
 * Starts one state of the actor a type-42 sensor drives. With no `state` the slot's target must
 * declare exactly one state; a generated `state` row must belong to this slot.
 */
[[nodiscard]] int slot_play_performance(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_performance_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-42 performance sensor");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 1> kDeclared{"state"};
    refuse_unknown_arguments(state, kDeclared);
    lua_Integer slotRow = static_cast<lua_Integer>(definition.nativeRow);
    lua_Integer nameHash = 0;
    if (!lua_isnoneornil(state, 2)) {
        lua_getfield(state, 2, "state");
        if (!lua_isnil(state, -1)) {
            luaL_checktype(state, -1, LUA_TTABLE);
            slotRow = directive_integer(state, -1, "slot_row");
            nameHash = directive_integer(state, -1, "name_hash");
        }
        lua_pop(state, 1);
    }
    if (slotRow < 0 || slotRow > (std::numeric_limits<std::uint32_t>::max)() || nameHash < 0
        || nameHash > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "performance state declaration is outside its native field width");
    }
    Impl* const impl = impl_from_state(state);
    PerformanceStateDefinition resolved{};
    if (impl == nullptr || impl->definitions.resolvePerformanceState == nullptr
        || !impl->definitions.resolvePerformanceState(impl->definitions.context,
                                                      static_cast<std::uint32_t>(slotRow),
                                                      static_cast<std::uint32_t>(nameHash),
                                                      resolved)
        || resolved.slotRow != definition.nativeRow) {
        return luaL_error(state, "performance state does not belong to this slot");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::playPerformance;
    intent.firstRow = definition.nativeRow;
    intent.secondRow = resolved.nameHash;
    return queue_intent(state, frame, intent);
}

/** Fires one bounded cue from an exact type-53 authored dialogue list. */
[[nodiscard]] int slot_play_dialogue_cue(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_dialogue_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-53 authored dialogue");
    }
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 1> kDeclared{"cue"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer cue = checked_integer_argument(state, "cue");
    if (cue < 0 || cue > (std::numeric_limits<std::uint16_t>::max)()) {
        return luaL_error(state, "dialogue cue must be a non-negative 16-bit integer");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::playDialogueCue;
    intent.firstRow = definition.nativeRow;
    intent.secondRow = static_cast<std::uint32_t>(cue);
    return queue_intent(state, frame, intent);
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
    } else if (key == "set_engagement_state") {
        lua_pushcfunction(state, &slot_set_engagement_state);
    } else if (key == "set_public_event_state") {
        lua_pushcfunction(state, &slot_set_public_event_state);
    } else if (key == "assign_combat_objective") {
        lua_pushcfunction(state, &slot_assign_combat_objective);
    } else if (key == "play_actor_path") {
        lua_pushcfunction(state, &slot_play_actor_path);
    } else if (key == "deliver_squad") {
        lua_pushcfunction(state, &slot_deliver_squad);
    } else if (key == "deliver_squads") {
        lua_pushcfunction(state, &slot_deliver_squads);
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
    } else if (key == "play_sequence") {
        lua_pushcfunction(state,
                          exact_combatant_slot(definition) ? &slot_play_actor_sequence
                                                           : &slot_play_sequence);
    } else if (key == "sequences") {
        lua_pushcfunction(state, &slot_actor_sequences);
    } else if (key == "set_scene_events") {
        lua_pushcfunction(state, &slot_set_scene_events);
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
