#pragma once

#include "../../state/activity/mission/definition.h"
#include "../../state/activity_sdk/generated_world/runtime.h"
#include "../../state/activity_sdk/runtime.h"

namespace sunrise::server::activity::actor_sequences {

using Owner = state::activity::mission::ActorSequenceOwner;

/** Resolves a combatant's exact authored squad and its single consistent actor class. */
[[nodiscard]] bool
owner(const state::activity_sdk::BoundView& view, std::uint32_t slotRow, Owner& output) noexcept;
[[nodiscard]] bool owner(const state::activity_sdk::generated_world::GeneratedWorldView& world,
                         std::uint32_t slotRow,
                         Owner& output) noexcept;

} // namespace sunrise::server::activity::actor_sequences
