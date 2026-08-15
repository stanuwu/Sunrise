#pragma once

#include "definition.h"

namespace sunrise::state::unlocks {

/**
 * Publishes the immutable unlock policy for this process.
 * @param table Complete authored policy.
 */
void publish(const Table& table) noexcept;

/** @return The active unlock policy, or an empty policy when none was published. */
[[nodiscard]] const Table& get() noexcept;

/** Restores the empty unlock policy. */
void clear() noexcept;

} // namespace sunrise::state::unlocks
