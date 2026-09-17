#pragma once

#include <cstddef>
#include <limits>

#include "../../core/network_capacity.h"

namespace sunrise::state {
/** Index into the account table; slot 0 is reserved for this installation's own account. */
using AccountHandle = std::size_t;
/** This installation's own account, never an enrolled remote profile. */
inline constexpr AccountHandle kLocalAccount = 0;
/** Returned by every lookup that resolved nothing. */
inline constexpr AccountHandle kInvalidAccount = (std::numeric_limits<AccountHandle>::max)();
/** The playing host occupies one of the shared session's player slots. */
inline constexpr std::size_t kAccountCapacity = core::network_capacity::kPlayers;
} // namespace sunrise::state
