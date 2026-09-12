#pragma once

#include "../../../state/activity_sdk/generated_world/runtime.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission::sdk_bridge {
void attach_actor_sequences(
    lua_vm::DefinitionApi& output,
    const state::activity_sdk::generated_world::GeneratedWorldView& world) noexcept;
}
