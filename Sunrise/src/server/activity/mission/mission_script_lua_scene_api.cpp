#include <string_view>

#include "../../../middleware/bap/activity_message/scene_events_auth.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Lua index for a scene handle: its named fields. Errors when the scene is stale. */
[[nodiscard]] int scene_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale");
    }
    const std::string_view key = lua_string_view(state, 2);
    if (key == "row") {
        lua_pushinteger(state, definition.localRow);
    } else if (key == "id") {
        lua_pushlstring(state, definition.id.data(), definition.idLength);
    } else if (key == "activate") {
        lua_pushcfunction(state, &scene_activate);
    } else if (key == "stop") {
        lua_pushcfunction(state, &scene_stop);
    } else if (key == "send_event") {
        lua_pushcfunction(state, &scene_send_event);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Stages one authored-scene activation. scene:activate{} reads no parameters. */
[[nodiscard]] int scene_activate(lua_State* state) {
    const auto* const handle =
        static_cast<const SceneHandle*>(luaL_checkudata(state, 1, kSceneMetatable));
    SceneDefinition definition{};
    if (!current_scene(state, *handle, definition)) {
        return luaL_error(state, "authored scene is stale or invalid");
    }
    refuse_unknown_arguments(state, {});
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::activateAuthoredScene;
    intent.firstRow = definition.occurrenceRow;
    intent.secondRow = definition.slotRow;
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
    static constexpr std::array<std::string_view, 1> kDeclared{"key"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer key = checked_integer_argument(state, "key");
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
    refuse_unknown_arguments(state, {});
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
