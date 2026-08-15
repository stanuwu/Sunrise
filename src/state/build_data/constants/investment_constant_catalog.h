#pragma once

#include "definition.h"

namespace sunrise::state::build_data::constants {

/** Clears the published investment constants. */
void clear() noexcept;

/**
 * Publishes one extracted constants row.
 * @param value Constants read from the installed blob.
 * @return True when the row is marked extracted.
 */
[[nodiscard]] bool replace(const InvestmentConstants& value) noexcept;

/** @param value Receives the published constants. @return True when a row is published. */
[[nodiscard]] bool find(InvestmentConstants& value) noexcept;

/** @return Copy read under the lock, cleared when nothing is published. */
[[nodiscard]] InvestmentConstants snapshot() noexcept;

} // namespace sunrise::state::build_data::constants
