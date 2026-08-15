#pragma once

#include "definition.h"

namespace sunrise::state::entitlements {

/**
 * Publishes the immutable ownership policy for this process.
 * @param table Complete authored policy.
 * @return False when the table is invalid. The old policy is kept.
 */
[[nodiscard]] bool publish(const Table& table) noexcept;

/** @return The active ownership policy, or the bundled policy when none was published. */
[[nodiscard]] const Table& get() noexcept;

/** Restores the bundled ownership policy. */
void clear() noexcept;

} // namespace sunrise::state::entitlements
