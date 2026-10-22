#pragma once

#include <cstddef>
#include <cstdint>

#include "../../state/activity/definition.h"

namespace sunrise::server::bap {
struct Session;
/** Resolves the exact globally allocated client generation. Caller holds the BAP session lock. */
[[nodiscard]] const Session*
activity_link_for_generation_locked(const state::activity::SessionBinding& binding,
                                    std::uint64_t generation,
                                    std::size_t& count) noexcept;

/**
 * Tests one link as a joined carrier for the binding's single mission program: authenticated, on
 * the exact binding and destination, and, when private, joined for its current generation. Caller
 * holds the BAP session lock.
 */
[[nodiscard]] bool
joined_mission_link_locked(const Session& session,
                           const state::activity::SessionBinding& binding) noexcept;

/**
 * Selects a joined transport for the one shared mission program, without changing gameplay
 * authority.
 */
[[nodiscard]] const Session*
mission_activity_link_locked(const state::activity::SessionBinding& binding,
                             std::size_t& count) noexcept;
} // namespace sunrise::server::bap
