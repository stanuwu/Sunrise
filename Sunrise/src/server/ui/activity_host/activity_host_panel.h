#pragma once
namespace sunrise::server::ui::activity_host {
/** Main-menu launcher for Activity Host tools. */
void draw() noexcept;
/** Both launched tabs use the same validated Activity Host selection. */
void draw_world() noexcept;
void draw_packets() noexcept;
void deactivate_world() noexcept;
} // namespace sunrise::server::ui::activity_host
