#pragma once

#include "../../../state/activity_sdk/runtime.h"

namespace sunrise::server::ui::activity_host::sdk_state_pages {

/** Draws the type-42 performance sensors and the authored state each can start. */
void draw_performances(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the type-70 engagement sensors and their flags and revision. */
void draw_engagement(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the type-71 public-event sensors and the watched player, area and timeout. */
void draw_public_events(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the type-30 occupancy conditions, their filter slot and caller value. */
void draw_occupancy(const state::activity_sdk::BoundView& view) noexcept;

/** Draws combatant bindings, retained channels and named sequence requests. */
void draw_combatants(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the scenario's authored states and the host select action for one region. */
void draw_states(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the mission record's durable variables and timers. Read-only. */
void draw_mission_state(const state::activity_sdk::BoundView& view) noexcept;

/** Draws the type-17 activity lifetime states and the set action. */
void draw_lifetime(const state::activity_sdk::BoundView& view) noexcept;

} // namespace sunrise::server::ui::activity_host::sdk_state_pages
