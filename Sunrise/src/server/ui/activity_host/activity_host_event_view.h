#pragma once

namespace sunrise::state::activity {
struct SessionBinding;
}

namespace sunrise::server::activity::host {
struct DiagnosticsSnapshot;
struct InstanceSnapshot;
} // namespace sunrise::server::activity::host

namespace sunrise::server::ui::activity_host::event_view {

/** Draws the movable packet, host-transition, and incident view. */
void draw(const server::activity::host::InstanceSnapshot* instance,
          const server::activity::host::DiagnosticsSnapshot& snapshot) noexcept;

} // namespace sunrise::server::ui::activity_host::event_view
