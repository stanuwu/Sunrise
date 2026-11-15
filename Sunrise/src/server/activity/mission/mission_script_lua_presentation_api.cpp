#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/music_section_auth.h"
#include "../activity_clock.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace auth_fields = middleware::bap::activity_message::auth_fields;

namespace {

// Table entries of the slot members this file implements.
constexpr const api::MemberSpec& kSetDirective = api::slot_member("set_directive");
constexpr const api::MemberSpec& kSetEngagement = api::slot_member("set_engagement");
constexpr const api::MemberSpec& kClearDirectives = api::slot_member("clear_directives");
constexpr const api::MemberSpec& kMusicSection = api::slot_member("set_music_section");
constexpr const api::MemberSpec& kPublicEvent = api::slot_member("set_public_event_state");
constexpr const api::MemberSpec& kPlaySequence =
    api::slot_member("play_sequence", format::kSequenceSlotType);
constexpr const api::MemberSpec& kCinematicActive = api::slot_member("set_cinematic_active");
constexpr const api::MemberSpec& kResetObjectives = api::slot_member("reset_objectives");
constexpr const api::MemberSpec& kAdvanceTask = api::slot_member("advance_task");
constexpr const api::MemberSpec& kPerformance = api::slot_member("play_performance");
constexpr const api::MemberSpec& kDialogueCue = api::slot_member("play_dialogue_cue");

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

/** @return True when one live Slot row is the exact type-68 HUD directive state. */
[[nodiscard]] bool exact_directive_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType68SlotType
           && definition.authSchema == scriptable_auth::kType68Schema
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

} // namespace

/**
 * Stages audience admission: the occupants of the volumes, and with `everyone` every present
 * player. The native flag bit 0 suppresses the client's player-table admission; the shipped
 * default keeps it set, so only volume occupants enter unless the script asks for everyone.
 * @param state Borrowed Lua call with a current sensor and named volumes/clear arguments.
 * @return One RequestKey; invalid inputs raise a Lua error before an intent is queued.
 */
[[nodiscard]] int slot_set_engagement(lua_State* state) {
    const SlotHandle* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kSetEngagement);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || slot.slotType != scriptable_auth::kType70SlotType
        || slot.componentClass != scriptable_auth::kType70ComponentClass
        || slot.authSchema != scriptable_auth::kType70Schema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return luaL_error(state, "engagement requires an exact type-70 sensor");
    }
    const bool clear = optional_boolean_argument(state, boolean_option(kSetEngagement, "clear"));
    static_cast<void>(
        push_argument(state, required_key(kSetEngagement, "volumes", api::ValueKind::list)));
    luaL_checktype(state, -1, LUA_TTABLE);
    refuse_unknown_fields(state, -1, {});
    const std::size_t count = lua_rawlen(state, -1);
    scriptable_auth::Type70Preset preset{};
    if (count > preset.volumes.size()) {
        return luaL_error(state, "engagement volume count exceeds native capacity");
    }
    // The Host replaces this template revision: one asks it to clear the audience, zero keeps it.
    preset.revision = clear ? 1 : 0;
    preset.volumeCount = static_cast<std::uint8_t>(count);
    if (optional_boolean_argument(state, boolean_option(kSetEngagement, "everyone"))) {
        preset.flags = scriptable_auth::kType70AdmitEveryoneFlags;
    }
    for (std::size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        const SlotHandle* const volumeHandle =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition volume{};
        if (!current_slot(state, *volumeHandle, volume)
            || volume.slotType != scriptable_auth::kType60SlotType
            || volume.objectTag != slot.objectTag || volume.registryKey != slot.registryKey
            || volume.slotIndex > auth_fields::kMaximumClientRefIndex) {
            return luaL_error(state, "engagement requires current same-owner type-60 volumes");
        }
        preset.volumes[index] = {volume.registryKey,
                                 static_cast<std::int8_t>(scriptable_auth::kType60SlotType),
                                 static_cast<std::int16_t>(volume.slotIndex)};
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    std::array<std::byte, scriptable_auth::kType70MaximumByteCount> body{};
    std::size_t written = 0;
    std::size_t bitCount = 0;
    if (!scriptable_auth::encode_type70(preset, body, written, bitCount)) {
        return luaL_error(state, "engagement native encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           scriptable_auth::kType70Schema,
                           bitCount,
                           std::span(body).first(written),
                           IntentKind::setEngagement);
}

/** Shows one generated directive through the exact type-68 Auth schema. */
[[nodiscard]] int slot_set_directive(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kSetDirective);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot)) {
        return luaL_error(state, "activity slot is stale or invalid");
    }
    if (!exact_directive_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-68 directive state");
    }
    using Kind = api::ValueKind;
    lua_getfield(state, 2, required_key(kSetDirective, "directive", Kind::missionRow));
    luaL_checktype(state, -1, LUA_TTABLE);
    const lua_Integer slotRow = directive_integer(
        state, -1, table_field_key(kSetDirective, "directive", "slot_row", Kind::integer));
    const lua_Integer nameHash = directive_integer(
        state, -1, table_field_key(kSetDirective, "directive", "name_hash", Kind::integer));
    const lua_Integer element = directive_integer(
        state, -1, table_field_key(kSetDirective, "directive", "element", Kind::integer));
    lua_pop(state, 1);
    const lua_Integer directiveState =
        optional_integer_argument(state, integer_option(kSetDirective, "state"));
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
    scriptable_auth::Type68Preset preset{
        .nameHash = resolved.nameHash,
        .elementIndex = resolved.elementIndex,
        .state = static_cast<std::int8_t>(directiveState),
        .visible = true,
        .phase = static_cast<std::int8_t>(
            optional_boolean_argument(state, boolean_option(kSetDirective, "active")) ? 1 : 0)};
    if (optional_boolean_argument(state, boolean_option(kSetDirective, "settled"))) {
        if (preset.phase != 0) {
            return luaL_error(state, "directive settled excludes active");
        }
        preset.phase = -1;
    }
    // Progress rides only on an element that declares a counter; the HUD ignores it otherwise.
    if (push_argument(state, optional_key(kSetDirective, "progress", Kind::list)) != LUA_TNIL) {
        luaL_checktype(state, -1, LUA_TTABLE);
        const std::size_t count = lua_rawlen(state, -1);
        if ((resolved.flags & format::kDirectiveElementCounter) == 0) {
            return luaL_error(state, "directive progress requires an element with a counter");
        }
        if (count == 0 || count > preset.progress.size()) {
            return luaL_error(state, "directive progress holds one to four integers");
        }
        for (std::size_t index = 1; index <= count; ++index) {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
            if (!lua_isinteger(state, -1)) {
                return luaL_error(state, "directive progress values must be integers");
            }
            const lua_Integer value = lua_tointeger(state, -1);
            if (value < (std::numeric_limits<std::int32_t>::min)()
                || value > (std::numeric_limits<std::int32_t>::max)()) {
                return luaL_error(state, "directive progress is outside its native field width");
            }
            preset.progress[index - 1] = static_cast<std::int32_t>(value);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    const char* const navpointKey = optional_key(kSetDirective, "navpoint", Kind::handle);
    if (!optional_slot_reference(state,
                                 optional_key(kSetDirective, "audience", Kind::handle),
                                 scriptable_auth::kType70SlotType,
                                 preset.audience)) {
        return luaL_error(state,
                          "directive audience requires an authored type-70 engagement sensor");
    }
    if (!optional_slot_reference(
            state, navpointKey, scriptable_auth::kType47SlotType, preset.navpoint)) {
        return luaL_error(state, "directive navpoint requires a current authored type-47 slot");
    }
    if (!optional_slot_reference(state,
                                 optional_key(kSetDirective, "waypoint", Kind::handle),
                                 scriptable_auth::kType60SlotType,
                                 preset.waypoint)) {
        return luaL_error(state, "directive waypoint requires a current authored type-60 volume");
    }
    if (!optional_slot_reference(state,
                                 optional_key(kSetDirective, "event", Kind::handle),
                                 scriptable_auth::kType71SlotType,
                                 preset.event)) {
        return luaL_error(state, "directive event requires a current authored type-71 sensor");
    }
    // The clock counts whole seconds down from its total; the encoder refuses it when hidden.
    if (push_argument(state, optional_key(kSetDirective, "clock", Kind::table)) != LUA_TNIL) {
        luaL_checktype(state, -1, LUA_TTABLE);
        const lua_Integer remaining = directive_integer(
            state, -1, table_field_key(kSetDirective, "clock", "remaining", Kind::integer));
        const lua_Integer total = directive_integer(
            state, -1, table_field_key(kSetDirective, "clock", "total", Kind::integer));
        if (remaining < 0 || total < remaining
            || total > scriptable_auth::kType68MaximumClockSeconds) {
            return luaL_error(state, "directive clock needs 0 <= remaining <= total <= one year");
        }
        preset.clockRemainingSeconds = static_cast<std::int32_t>(remaining);
        preset.clockTotalSeconds = static_cast<std::int32_t>(total);
        if (!activity_clock_wire_ticks(impl->identity.sessionId,
                                       impl->identity.sessionCreatedRevision,
                                       active_frame(state).now,
                                       false,
                                       preset.clockEpochTicks)) {
            return luaL_error(state, "directive clock is outside the activity time domain");
        }
    }
    lua_pop(state, 1);
    if (preset.navpoint.slotIndex >= 0) {
        lua_getfield(state, 2, navpointKey);
        const auto* const navpoint =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition navpointSlot{};
        if (current_slot(state, *navpoint, navpointSlot)) {
            preset.navpointNameHash = navpointSlot.nameHash;
            preset.navpointBubbleHash = navpointSlot.bubbleHash;
        }
        lua_pop(state, 1);
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
    refuse_unknown_arguments(state, kClearDirectives);
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

/** Selects one authored section in a native music sensor's selection mask. */
[[nodiscard]] int slot_set_music_section(lua_State* state) {
    namespace music = middleware::bap::activity_message::music_section;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kMusicSection);
    SlotDefinition slot{};
    const lua_Integer section = checked_integer_argument(
        state, required_key(kMusicSection, "section", api::ValueKind::integer));
    if (!current_slot(state, *handle, slot) || slot.slotType != music::kSlotType
        || slot.componentClass != music::kComponentClass || slot.authSchema != music::kSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0 || section < 0
        || section >= static_cast<lua_Integer>(music::kSectionCount)) {
        return luaL_error(state, "music requires an exact type-11 sensor and a section index");
    }
    std::array<std::byte, music::kBytes> body{};
    std::size_t written = 0;
    if (!music::encode(static_cast<std::uint8_t>(section),
                       optional_boolean_argument(state, boolean_option(kMusicSection, "enabled")),
                       body,
                       written)) {
        return luaL_error(state, "music section encoder failed");
    }
    return queue_slot_auth(state, slot, music::kSchema, music::kBits, body);
}

/** Names the player, the event area and the leave timeout one public-event sensor watches. */
[[nodiscard]] int slot_set_public_event_state(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_public_event_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-71 public-event sensor");
    }
    refuse_unknown_arguments(state, kPublicEvent);
    using Kind = api::ValueKind;
    const lua_Integer eventState =
        optional_integer_argument(state, integer_option(kPublicEvent, "state"));
    if (eventState < (std::numeric_limits<std::int32_t>::min)()
        || eventState > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "state must be a 32-bit signed integer");
    }
    // Absent, the sensor watches this link's own player. Present, it is a decimal string, the
    // form every 64-bit key crosses into Lua in.
    std::uint64_t player = impl_from_state(state)->identity.playerKey;
    const char* const playerKey = optional_key(kPublicEvent, "player", Kind::decimalString);
    if (push_argument(state, playerKey) != LUA_TNIL) {
        lua_pop(state, 1);
        const std::string_view playerText = borrowed_string_argument(state, playerKey);
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
    const SlotHandle areaHandle = checked_argument<SlotHandle>(
        state, required_key(kPublicEvent, "area", Kind::handle), kSlotMetatable);
    const lua_Number seconds =
        checked_number_argument(state, required_key(kPublicEvent, "leave_seconds", Kind::number));
    SlotDefinition area{};
    // Restrict this API to authored trigger volumes until other spatial area types are supported.
    if (!current_slot(state, areaHandle, area)
        || area.slotType != scriptable_auth::kType60SlotType) {
        return luaL_error(state, "public-event area requires a current type-60 volume");
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

/** Lua `play_sequence` on a slot. Errors unless the slot is an exact type-5 sequence. */
[[nodiscard]] int slot_play_sequence(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_sequence_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-5 authored sequence");
    }
    refuse_unknown_arguments(state, kPlaySequence);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::playSequence;
    intent.firstRow = definition.nativeRow;
    if (lua_istable(state, 2)) {
        if (push_argument(state, optional_key(kPlaySequence, "choice", api::ValueKind::integer))
            != LUA_TNIL) {
            const lua_Integer choice = luaL_checkinteger(state, -1);
            constexpr auto kChoiceCount =
                static_cast<lua_Integer>(scriptable_auth::kType5ChoiceCount);
            if (choice < 0 || choice >= kChoiceCount) {
                return luaL_error(
                    state, "sequence choice must be 0 to %d", static_cast<int>(kChoiceCount - 1));
            }
            intent.sequenceChoice = static_cast<std::int32_t>(choice);
        }
        lua_pop(state, 1);
        intent.sequenceRetain =
            !optional_boolean_argument(state, boolean_option(kPlaySequence, "restart"));
    }
    if (intent.sequenceRetain && intent.sequenceChoice < 0) {
        return luaL_error(state, "play_sequence with restart = false needs a choice");
    }
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
    refuse_unknown_arguments(state, kCinematicActive);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setCinematicActive;
    intent.firstRow = definition.nativeRow;
    intent.active = optional_boolean_argument(state, boolean_option(kCinematicActive, "active"));
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
    refuse_unknown_arguments(state, kResetObjectives);
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
    refuse_unknown_arguments(state, kAdvanceTask);
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
    refuse_unknown_arguments(state, kPerformance);
    lua_Integer slotRow = static_cast<lua_Integer>(definition.nativeRow);
    lua_Integer nameHash = 0;
    if (!lua_isnoneornil(state, 2)) {
        using Kind = api::ValueKind;
        lua_getfield(state, 2, optional_key(kPerformance, "state", Kind::missionRow));
        if (!lua_isnil(state, -1)) {
            luaL_checktype(state, -1, LUA_TTABLE);
            slotRow = directive_integer(
                state, -1, table_field_key(kPerformance, "state", "slot_row", Kind::integer));
            nameHash = directive_integer(
                state, -1, table_field_key(kPerformance, "state", "name_hash", Kind::integer));
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
    refuse_unknown_arguments(state, kDialogueCue);
    const lua_Integer cue =
        checked_integer_argument(state, required_key(kDialogueCue, "cue", api::ValueKind::integer));
    if (cue < 0 || cue > (std::numeric_limits<std::uint16_t>::max)()) {
        return luaL_error(state, "dialogue cue must be a non-negative 16-bit integer");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::playDialogueCue;
    intent.firstRow = definition.nativeRow;
    intent.secondRow = static_cast<std::uint32_t>(cue);
    lua_getfield(state, 2, optional_key(kDialogueCue, "filter", api::ValueKind::handle));
    if (!lua_isnil(state, -1)) {
        const auto* const volume =
            static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
        SlotDefinition volumeSlot{};
        if (!current_slot(state, *volume, volumeSlot)
            || volumeSlot.slotType
                   != static_cast<std::uint32_t>(scriptable_auth::kType53FilterSlotType)) {
            return luaL_error(state, "dialogue filter requires a current authored type-60 volume");
        }
        intent.dialogueFilterRow = static_cast<std::int32_t>(volumeSlot.nativeRow);
    }
    lua_pop(state, 1);
    return queue_intent(state, frame, intent);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
