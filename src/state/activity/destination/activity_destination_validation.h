#pragma once

#include "definition.h"

namespace sunrise::state::activity::destination {

/** Bounds one destination selection. @return True when the name length fits the fixed array. */
[[nodiscard]] bool valid(const DestinationSelection& selection) noexcept;

} // namespace sunrise::state::activity::destination
