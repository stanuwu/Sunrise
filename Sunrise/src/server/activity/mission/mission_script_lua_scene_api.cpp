#include <array>
#include <string_view>

#include "../../../middleware/bap/activity_message/scene_events_auth.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Table entries of the scene members this file implements.
constexpr const api::MemberSpec& kActivate = api::member(api::Receiver::scene, "activate");
constexpr const api::MemberSpec& kSendEvent = api::member(api::Receiver::scene, "send_event");
constexpr const api::MemberSpec& kStop = api::member(api::Receiver::scene, "stop");

/** Scene members in table order. */
constexpr std::array<Binding<SceneDefinition>, std::size(api::kSceneMembers)> kSceneBindings{{
    {"row",
     nullptr,
     [](lua_State* state, const SceneDefinition& scene) {
         lua_pushinteger(state, scene.localRow);
     }},
    {"id",
     nullptr,
     [](lua_State* state, const SceneDefinition& scene) {
         lua_pushlstring(state, scene.id.data(), scene.idLength);
     }},
    {"activate", &scene_activate},
    {"stop", &scene_stop},
    {"send_event", &scene_send_event},
}};
static_assert(bound_to<SceneDefinition>(api::kSceneMembers, kSceneBindings));

} // namespace

/** Lua index for a scene handle: its named fields. Errors when the scene is stale. */
[[nodiscard]] int scene_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale");
    }
    return dispatch<SceneDefinition>(state, kSceneBindings, lua_string_view(state, 2), definition);
}

/** Scene creation records exact SDK sources before native preparation can change them. */
[[nodiscard]] int scene_activate(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale or invalid");
    }
    refuse_unknown_arguments(state, kActivate);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::activateAuthoredScene;
    intent.firstRow = definition.occurrenceRow;
    intent.secondRow = definition.slotRow;
    intent.active = optional_boolean_argument(state, boolean_option(kActivate, "spawn"));
    if (push_argument(state, optional_key(kActivate, "initial_event", api::ValueKind::integer))
        != LUA_TNIL) {
        const lua_Integer key = luaL_checkinteger(state, -1);
        if (key <= 0
            || key >= static_cast<lua_Integer>(
                   middleware::bap::activity_message::scene_events::kInvalidEventKey)) {
            return luaL_error(state, "invalid initial scene event key");
        }
        intent.sceneEventKey = static_cast<std::uint32_t>(key);
    }
    lua_pop(state, 1);
    if (intent.active) {
        const auto& definitions = impl_from_state(state)->definitions;
        std::size_t count = 0;
        if (definitions.resolveSceneSpawnSources == nullptr
            || !definitions.resolveSceneSpawnSources(definitions.context,
                                                     definition.occurrenceRow,
                                                     definition.slotRow,
                                                     intent.burstRows,
                                                     count)
            || count > intent.burstRows.size()) {
            return luaL_error(state, "scene has no unambiguous authored spawn cast");
        }
        intent.burstRowCount = static_cast<std::uint8_t>(count);
    }
    return queue_intent(state, frame, intent);
}

/** Adds one event to the current scene generation after its activation reaches transport. */
[[nodiscard]] int scene_send_event(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale or invalid");
    }
    refuse_unknown_arguments(state, kSendEvent);
    const lua_Integer key =
        checked_integer_argument(state, required_key(kSendEvent, "key", api::ValueKind::integer));
    if (key <= 0
        || key >= static_cast<lua_Integer>(
               middleware::bap::activity_message::scene_events::kInvalidEventKey)) {
        return luaL_error(state, "invalid scene event key");
    }
    Intent intent{};
    intent.kind = IntentKind::signalAuthoredScene;
    intent.firstRow = definition.occurrenceRow;
    intent.secondRow = definition.slotRow;
    intent.sceneEventKey = static_cast<std::uint32_t>(key);
    return queue_intent(state, active_frame(state), intent);
}

/** Stops the current scene generation without creating another activation. */
[[nodiscard]] int scene_stop(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale or invalid");
    }
    refuse_unknown_arguments(state, kStop);
    Intent intent{};
    intent.kind = IntentKind::stopAuthoredScene;
    intent.firstRow = definition.occurrenceRow;
    intent.secondRow = definition.slotRow;
    return queue_intent(state, active_frame(state), intent);
}

void register_scene_metatables(lua_State* state) {
    register_metatable(state, kSceneMetatable, &scene_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
