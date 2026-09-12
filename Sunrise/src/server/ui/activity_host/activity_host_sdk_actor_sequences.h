#pragma once

#include "../../../state/activity_sdk/runtime.h"

namespace sunrise::server::ui::activity_host::sdk_state_pages {
void draw_actor_sequences(const state::activity_sdk::BoundView& view,
                          std::uint32_t slotRow) noexcept;
}
