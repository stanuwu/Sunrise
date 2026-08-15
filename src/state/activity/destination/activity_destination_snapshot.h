#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::activity::destination {

/**
 * Copies the destination committed with one activity session.
 * @param sessionId Nonzero id returned by a committed allocation.
 * @param output Cleared first, then gets one bounded scalar selection.
 * @return True when the bounded table still holds the session.
 */
[[nodiscard]] bool snapshot(std::uint64_t sessionId, DestinationSelection& output) noexcept;

} // namespace sunrise::state::activity::destination
