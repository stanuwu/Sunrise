#pragma once

#include "../../modules/ui_module_descriptor.h"
#include "../layout.h"

namespace sunrise::core::ui::layout::navigation {

/** Selected descriptor copied out of one registry snapshot. */
struct Selection {
    modules::Descriptor descriptor;
    bool moduleAvailable{};
};

/**
 * Draws the narrow module menu without retaining registry storage.
 * @param state Layout selection captured at frame start.
 * @return Copied module descriptor for the wide content panel.
 */
[[nodiscard]] Selection draw(const StateSnapshot& state) noexcept;

} // namespace sunrise::core::ui::layout::navigation
