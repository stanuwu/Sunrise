#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../middleware/bap/activity_message/combatant_auth.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace format = state::activity_sdk::format;
namespace scriptable_auth = middleware::bap::activity_message::scriptable_auth;
namespace auth_fields = middleware::bap::activity_message::auth_fields;

namespace {

// Table entries of the actor members this file implements.
constexpr const api::MemberSpec& kActorPath = api::slot_member("play_actor_path");
constexpr const api::MemberSpec& kActorAction = api::slot_member("play_actor_action");
constexpr const api::MemberSpec& kRetireActor = api::slot_member("retire_actor");
constexpr const api::MemberSpec& kBindCombatant = api::slot_member("bind_combatant_to_squad");
constexpr const api::MemberSpec& kRunAtoms = api::slot_member("run_atoms");

/** Native state replaces the template counters before publishing an actor program. */
[[nodiscard]] int queue_actor_template(lua_State* state,
                                       const SlotDefinition& actor,
                                       scriptable_auth::Type2Body& program,
                                       IntentKind kind,
                                       bool spawn) {
    // The encoder needs a positive placeholder before native revision assignment.
    constexpr std::uint32_t kTemplateRevision = 1;
    program.channels.revision = kTemplateRevision;
    program.atoms.generation = kTemplateRevision;
    std::array<std::byte, scriptable_auth::kType2MaximumBodyByteCount> body{};
    std::size_t written = 0, bits = 0;
    if (!scriptable_auth::encode_type2_body(program, body, written, bits)) {
        return luaL_error(state, "actor program encoder failed");
    }
    return queue_slot_auth(state,
                           actor,
                           scriptable_auth::kType2Schema,
                           bits,
                           std::span(body).first(written),
                           kind,
                           spawn);
}

} // namespace

/** @return True when one live Slot row is an exact type-2 combatant. */
[[nodiscard]] bool exact_combatant_slot(const SlotDefinition& definition) noexcept {
    return definition.slotType == scriptable_auth::kType2SlotType
           && definition.componentClass == scriptable_auth::kType2ComponentClass
           && definition.senseSchema == scriptable_auth::kType2SenseSchema
           && definition.authSchema == scriptable_auth::kType2Schema
           && (definition.flags & format::kSlotSchemaJoinExact) != 0;
}

/** Follows an authored path while native state owns actor creation and program revisions. */
[[nodiscard]] int slot_play_actor_path(lua_State* state) {
    namespace combatant = middleware::bap::activity_message::combatant_auth;
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kActorPath);
    const auto reference = checked_argument<SlotHandle>(
        state, required_key(kActorPath, "path", api::ValueKind::handle), kSlotMetatable);
    SlotDefinition actor{}, path{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)
        || !current_slot(state, reference, path) || path.slotType != combatant::kPathSlotType
        || path.componentClass != combatant::kAbilityPointComponentClass
        || (path.flags & format::kSlotSchemaJoinExact) == 0 || actor.objectTag != path.objectTag
        || actor.registryKey != path.registryKey
        || path.slotIndex > auth_fields::kMaximumClientRefIndex) {
        return luaL_error(state, "actor path requires an exact same-registry authored path");
    }
    const Impl* impl = impl_from_state(state);
    if (impl->definitions.resolveActorAbilityTarget == nullptr
        || !impl->definitions.resolveActorAbilityTarget(
            impl->definitions.context, path.nativeRow, combatant::kPathDestinationMarker)) {
        return luaL_error(state, "actor path has no authored destination parameter");
    }
    scriptable_auth::Type2Body program{};
    program.atoms.count = 1;
    program.atoms.lanes[0].primary = scriptable_auth::Type2LaneRefByteBool{
        {path.registryKey,
         static_cast<std::int8_t>(path.slotType),
         static_cast<std::int16_t>(path.slotIndex)},
        static_cast<std::uint8_t>(combatant::kPathDestinationMarker),
        true};
    return queue_actor_template(
        state,
        actor,
        program,
        IntentKind::runActorProgram,
        optional_boolean_argument(state, boolean_option(kActorPath, "spawn")));
}

/** Runs one extracted actor ability through the shared native program constructor. */
[[nodiscard]] int slot_play_actor_action(lua_State* state) {
    refuse_unknown_arguments(state, kActorAction);
    using Kind = api::ValueKind;
    // The one-atom program carries this call's spawn, ability and target unchanged.
    constexpr const api::AtomKindSpec& kAbilityAtom = api::find_atom_kind("ability");
    lua_createtable(state, 0, 2);
    const int arguments = lua_gettop(state);
    lua_getfield(state, 2, optional_key(kActorAction, "spawn", Kind::boolean));
    lua_setfield(state, arguments, optional_key(kRunAtoms, "spawn", Kind::boolean));
    lua_createtable(state, 1, 0);
    const int atoms = lua_gettop(state);
    lua_createtable(state, 0, 3);
    lua_pushlstring(state, kAbilityAtom.name.data(), kAbilityAtom.name.size());
    lua_setfield(state, -2, api::kAtomKindField.name.data());
    lua_getfield(state, 2, required_key(kActorAction, "ability", Kind::missionRow));
    lua_setfield(state, -2, api::find_field(kAbilityAtom.fields, "ability").name.data());
    lua_getfield(state, 2, optional_key(kActorAction, "target", Kind::handle));
    lua_setfield(state, -2, api::find_field(kAbilityAtom.fields, "target").name.data());
    lua_rawseti(state, atoms, 1);
    lua_setfield(state, arguments, required_key(kRunAtoms, "atoms", Kind::list));
    lua_replace(state, 2);
    lua_settop(state, 2);
    return slot_run_atoms(state);
}

/** Retires one named actor without accepting a script-owned generation. */
[[nodiscard]] int slot_retire_actor(lua_State* state) {
    const auto* handle = static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kRetireActor);
    SlotDefinition actor{};
    if (!current_slot(state, *handle, actor) || !exact_combatant_slot(actor)) {
        return luaL_error(state, "actor retirement requires an exact actor slot");
    }
    scriptable_auth::Type2Body program{};
    return queue_actor_template(state, actor, program, IntentKind::retireActor, false);
}

/**
 * Arms one exact combatant for its scene's squad member spawn.
 * @param state Lua call holding the slot and empty argument table.
 * @return One request handle after the intent is retained.
 */
[[nodiscard]] int slot_bind_combatant_to_squad(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    refuse_unknown_arguments(state, kBindCombatant);
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

} // namespace sunrise::server::activity::mission::lua_vm::detail
