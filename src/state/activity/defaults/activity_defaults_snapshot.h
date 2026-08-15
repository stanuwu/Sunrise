#pragma once

#include "definition.h"

namespace sunrise::state::activity::defaults {

/** Copies the immutable activity defaults published with the root State. */
void snapshot(ActivityDefaults& output) noexcept;

/**
 * Applies any authored arrival override for one destination onto its selection.
 * @param defaults Authored defaults carrying the override table.
 * @param selection Committed destination, updated in place when a row names it.
 */
void apply_arrival_override(const ActivityDefaults& defaults,
                            destination::DestinationSelection& selection) noexcept;

} // namespace sunrise::state::activity::defaults
