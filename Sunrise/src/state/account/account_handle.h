#pragma once

#include <cstddef>
#include <limits>

#include "../../core/network_capacity.h"

namespace sunrise::state {
using AccountHandle = std::size_t;
inline constexpr AccountHandle kLocalAccount = 0;
inline constexpr AccountHandle kInvalidAccount = (std::numeric_limits<AccountHandle>::max)();
/** The playing host occupies one of the shared session's player slots. */
inline constexpr std::size_t kAccountCapacity = core::network_capacity::kPlayers;
} // namespace sunrise::state
