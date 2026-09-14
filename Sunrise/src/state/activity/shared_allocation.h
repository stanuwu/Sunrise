#pragma once

#include "definition.h"
#include "launch_party.h"

namespace sunrise::state::activity {
/** Prepares an account-owned native launch, sharing only with corroborated party members. */
[[nodiscard]] bool prepare_shared_session(const destination::DestinationSelection& selection,
                                          const LaunchParty& party,
                                          std::uint64_t& sessionId,
                                          PendingAllocation& allocation) noexcept;
[[nodiscard]] bool prepare_shared_session(const LaunchParty& party,
                                          std::uint64_t& sessionId,
                                          PendingAllocation& allocation) noexcept;

namespace transactions {
/** The public commit entry consumes the allocation before dispatching here. */
[[nodiscard]] bool commit_shared_allocation(const PendingAllocation& plan) noexcept;
} // namespace transactions
} // namespace sunrise::state::activity
