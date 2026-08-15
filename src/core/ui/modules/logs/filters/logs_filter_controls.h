#pragma once

#include "../../../../logging/view/log_snapshot_view.h"

namespace sunrise::core::ui::modules::logs::internal::filters {

/** Draws the exact-channel selector and stores the selection. */
void draw_channel() noexcept;

/** Draws the exact-level selector and stores the selection. */
void draw_level() noexcept;

/** Draws the text query input and stores the query. */
void draw_text() noexcept;

/** @return A filter built from the current selections. */
[[nodiscard]] log::view::Filter current() noexcept;

/** Clears every selection at a UI lifecycle boundary. */
void reset() noexcept;

} // namespace sunrise::core::ui::modules::logs::internal::filters
