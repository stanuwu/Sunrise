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
} // namespace sunrise::server::bap
