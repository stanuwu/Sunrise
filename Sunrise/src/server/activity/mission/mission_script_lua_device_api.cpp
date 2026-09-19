#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../../middleware/encoding/bit_writer.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace auth_fields = middleware::bap::activity_message::auth_fields;

namespace {

/** Templates need a positive counter; the native reducer chooses the transported value. */
constexpr std::int32_t kUncommittedCounter = 1;

/** @return True when one live Slot row is an exact type-23 device. */
[[nodiscard]] bool exact_device_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kDeviceSlotType
           && definition.componentClass == format::kDeviceComponentClass
           && definition.senseSchema == format::kDeviceSenseSchema
           && definition.authSchema == format::kDeviceAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-4 authored object. */
[[nodiscard]] bool exact_object_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kObjectSlotType
           && definition.componentClass == format::kObjectComponentClass
           && definition.senseSchema == format::kObjectSenseSchema
           && definition.authSchema == format::kObjectAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-65 Ghost-link sensor. */
[[nodiscard]] bool exact_ghost_link_slot(const SlotDefinition& definition) noexcept {
    namespace ghost = middleware::bap::activity_message::ghost_link;
    return definition.slotType == ghost::kSlotType
           && definition.componentClass == ghost::kComponentClass
           && definition.senseSchema == ghost::kSenseSchema
           && definition.authSchema == ghost::kAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-31 configured trigger. */
[[nodiscard]] bool exact_trigger_slot(const SlotDefinition& definition) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    return definition.slotType == auth::kType31SlotType
           && definition.authSchema == auth::kType31Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when one live Slot row is an exact type-30 occupancy condition. */
[[nodiscard]] bool exact_occupancy_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == format::kOccupancySlotType
           && definition.componentClass == format::kOccupancyComponentClass
           && definition.senseSchema == format::kOccupancySenseSchema
           && definition.authSchema == format::kOccupancyAuthSchema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
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

/** The occupancy Auth body is a fixed 87 bits: a 55-bit client reference then one int32. */
constexpr std::size_t kOccupancyAuthBitCount = 87;
constexpr std::size_t kOccupancyAuthByteCount = 11;

/** Volumes one filter may test; leaves room for the players, target and inside predicates. */
constexpr std::size_t kMaximumFilterVolumes = 5;
/** Type-34 predicate modes: 0 tests the flag or reference as given, 1 tests inside a volume. */
constexpr std::int8_t kFilterModeDirect = 0;
constexpr std::int8_t kFilterModeInside = 1;

} // namespace

/**
 * Sets the object filter and caller value carried by one type-30 condition. Without a filter the
 * reference is absent, and the client measures its default player set.
 */
[[nodiscard]] int slot_set_occupancy_condition(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Named arguments this call accepts. Any other key is refused.
    static constexpr std::array<std::string_view, 2> kDeclared{"filter", "value"};
    refuse_unknown_arguments(state, kDeclared);
    SlotHandle reference{};
    const bool filtered = optional_argument(state, "filter", kSlotMetatable, reference);
    const lua_Integer value = checked_integer_argument(state, "value");
    SlotDefinition slot{};
    SlotDefinition playerSet{};
    if (!current_slot(state, *handle, slot)
        || (filtered && !current_slot(state, reference, playerSet))) {
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
    const bool referenceWritten =
        filtered ? writer.write(playerSet.registryKey, 32)
                       && writer.write(static_cast<std::uint32_t>(playerSet.slotType) + 1U, 7)
                       && writer.write(static_cast<std::uint32_t>(playerSet.slotIndex) + 32768U, 16)
                 : auth_fields::write_absent_client_ref(writer);
    if (!referenceWritten || !writer.write(encodedValue, 32)
        || writer.bit_count() != kOccupancyAuthBitCount) {
        return luaL_error(state, "occupancy condition native encoder failed");
    }
    return queue_slot_auth(state, slot, format::kOccupancyAuthSchema, kOccupancyAuthBitCount, body);
}

/** Enables the native darkness restriction; roster assembly supplies its matching bubble. */
[[nodiscard]] int slot_set_darkness_zone(lua_State* state) {
    namespace darkness = middleware::bap::activity_message::darkness_zone;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
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

/** Native typed object filters: players, one object, and volume intersection. */
[[nodiscard]] int slot_set_object_filter(lua_State* state) {
    namespace auth = scriptable_auth;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 6> kDeclared{
        "players", "target", "targets", "squad", "inside", "inside_any"};
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
    // Several authored objects at once (a beam hop-on names every crystal it tethers).
    lua_getfield(state, 2, "targets");
    if (!lua_isnil(state, -1)) {
        luaL_checktype(state, -1, LUA_TTABLE);
        const std::size_t count = lua_rawlen(state, -1);
        if (count == 0 || count + body.count > auth::kType34PredicateCapacity) {
            return luaL_error(state, "targets count is outside the filter capacity");
        }
        for (std::size_t index = 1; index <= count; ++index) {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
            const auto* const targetHandle =
                static_cast<const SlotHandle*>(luaL_checkudata(state, -1, kSlotMetatable));
            SlotDefinition object{};
            if (!current_slot(state, *targetHandle, object)
                || object.slotType != auth::kType4SlotType
                || object.slotIndex > auth_fields::kMaximumClientRefIndex) {
                return luaL_error(state, "targets require authored type-4 objects");
            }
            body.predicates[body.count++] =
                auth::Type34ModeSlotRefC{kFilterModeDirect,
                                         {object.registryKey,
                                          static_cast<std::int8_t>(auth::kType4SlotType),
                                          static_cast<std::int16_t>(object.slotIndex)}};
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    // A squad reference selects the boss a shield/tether hop-on hosts on.
    auth::Type2LaneClientRef squad{};
    if (!optional_slot_reference(state, "squad", 1U, squad)) {
        return luaL_error(state, "filter squad must be an authored type-1 squad");
    }
    if (squad.slotIndex >= 0) {
        if (body.count >= auth::kType34PredicateCapacity) {
            return luaL_error(state, "squad does not fit the filter capacity");
        }
        body.predicates[body.count++] = auth::Type34ModeSlotRefC{kFilterModeDirect, squad};
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

/** Binds an authored damage monitor to one exact object; a new revision re-binds it. */
[[nodiscard]] int slot_watch_damage(lua_State* state) {
    namespace damage = middleware::bap::activity_message::damage_monitor;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"target"};
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
    std::array<std::byte, damage::kBytes> body{};
    std::size_t written = 0;
    if (!damage::encode(target.registryKey,
                        static_cast<std::uint16_t>(target.slotIndex),
                        kUncommittedCounter,
                        body,
                        written)) {
        return luaL_error(state, "damage monitor encoder failed");
    }
    return queue_slot_auth(
        state, slot, damage::kAuthSchema, damage::kBits, body, IntentKind::watchDamage);
}

/** Spawns authored entry zero and subscribes to accepted native player use. */
[[nodiscard]] int slot_set_interactable_object(lua_State* state) {
    namespace object = middleware::bap::activity_message::interactable_object;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 3> kDeclared{"track_owner", "active", "used"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_object_slot(slot)) {
        return luaL_error(state, "interaction requires an exact authored object");
    }
    const bool trackOwner = optional_boolean_argument(state, "track_owner", false);
    std::array<std::byte, object::kOwnerBytes> body{};
    std::size_t written = 0;
    if (!object::encode(kUncommittedCounter,
                        body,
                        written,
                        trackOwner,
                        optional_boolean_argument(state, "active", true),
                        optional_boolean_argument(state, "used", false))) {
        return luaL_error(state, "interactable object encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           object::kSchema,
                           trackOwner ? object::kOwnerBits : object::kBits,
                           std::span(body).first(written),
                           IntentKind::setInteractableObject);
}

/** Arms or disarms the authored Ghost-link scan. The Host stamps the generation this body needs. */
[[nodiscard]] int slot_set_ghost_link(lua_State* state) {
    namespace ghost = middleware::bap::activity_message::ghost_link;
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only this named argument belongs to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"active"};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_ghost_link_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-65 Ghost-link sensor");
    }
    std::array<std::byte, ghost::kByteCount> body{};
    std::size_t written = 0;
    if (!ghost::encode(
            kUncommittedCounter, optional_boolean_argument(state, "active", true), body, written)) {
        return luaL_error(state, "Ghost-link encoder failed");
    }
    return queue_slot_auth(state,
                           slot,
                           ghost::kAuthSchema,
                           ghost::kBitCount,
                           std::span(body).first(written),
                           IntentKind::setGhostLink);
}

/**
 * Reads the level the client last reported for one Ghost link.
 * @return The generation, progress and active fields, or nil before all three were reported.
 */
[[nodiscard]] int slot_ghost_link(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // The read has no parameters, so an unsafe call cannot be spelled.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_ghost_link_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact type-65 Ghost-link sensor");
    }
    const Impl* const impl = impl_from_state(state);
    for (std::size_t index = 0; index < impl->ghostLevelCount; ++index) {
        const GhostLinkRow& row = impl->ghostLevels[index];
        if (row.registryKey != slot.registryKey || row.objectTag != slot.objectTag
            || static_cast<std::uint32_t>(row.slotIndex) != slot.slotIndex
            || row.level.seen != kGhostSeenAll) {
            continue;
        }
        lua_createtable(state, 0, 3);
        lua_pushinteger(state, row.level.generation);
        lua_setfield(state, -2, "generation");
        lua_pushnumber(state, row.level.progress);
        lua_setfield(state, -2, "progress");
        lua_pushboolean(state, row.level.active);
        lua_setfield(state, -2, "active");
        return 1;
    }
    lua_pushnil(state);
    return 1;
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
    intent.active = true;
    return queue_intent(state, frame, intent);
}

/** Disarms one type-31 trigger, so an arm after its first fire stops reporting. */
[[nodiscard]] int slot_disarm_trigger(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition definition{};
    if (!current_slot(state, *handle, definition) || !exact_trigger_slot(definition)) {
        return luaL_error(state, "activity slot is not an exact type-31 trigger");
    }
    // The disarm has no parameters, so any named argument is refused.
    static constexpr std::array<std::string_view, 0> kDeclared{};
    refuse_unknown_arguments(state, kDeclared);
    Intent intent{};
    intent.kind = IntentKind::fireTrigger;
    intent.firstRow = definition.nativeRow;
    intent.active = false;
    return queue_intent(state, active_frame(state), intent);
}

/**
 * A newer desired command hides every earlier applied result on the same device channel.
 * @param state Lua call holding the device slot and named channel argument.
 * @return True only after a report satisfied the latest desired request.
 */
int slot_applied(lua_State* state) {
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    // Only these named arguments belong to this API.
    static constexpr std::array<std::string_view, 1> kDeclared{"channel"};
    refuse_unknown_arguments(state, kDeclared);
    const auto channel =
        checked_argument<DeviceChannelHandle>(state, "channel", kDeviceChannelMetatable);
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot)
        || slot.slotType != ::sunrise::state::activity_sdk::format::kDeviceSlotType) {
        return luaL_error(state, "device completion requires an authored device slot");
    }
    const Impl* impl = impl_from_state(state);
    const auto pending = [&](const auto& intent) noexcept {
        return intent.kind == IntentKind::setDeviceChannel && intent.firstRow == slot.nativeRow
               && intent.deviceChannel == channel.channel
               && intent.attemptGeneration == impl->attempt.generation;
    };
    for (const auto& intent : active_frame(state).candidate.intents) {
        if (pending(intent)) {
            lua_pushboolean(state, false);
            return 1;
        }
    }
    for (std::size_t index = impl->outboxRead; index < impl->outbox.size(); ++index) {
        if (pending(impl->outbox[index])) {
            lua_pushboolean(state, false);
            return 1;
        }
    }
    bool applied = false;
    for (const auto& request : impl->deviceRequests) {
        if (request.slotRow == slot.nativeRow && request.channel == channel.channel
            && request.attemptGeneration == impl->attempt.generation
            && impl->deviceRequestGeneration != 0
            && request.sourceGeneration == impl->deviceRequestGeneration) {
            applied = request.applied;
            break;
        }
    }
    lua_pushboolean(state, applied);
    return 1;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
