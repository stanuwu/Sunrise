#pragma once

#include "definition.h"

namespace sunrise::state::activity {
/** Resolves an existing native activity only for an exact, committed peer reservation. */
[[nodiscard]] bool shared_target(std::uint64_t sessionId,
                                 const membership::Identity& identity,
                                 SessionBinding& binding) noexcept;
} // namespace sunrise::state::activity
