#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::activity::reservations {
/** The native session owner requests these already-enrolled players' actual selected characters. */
[[nodiscard]] bool prepare_admit(std::uint64_t sessionId,
                                 std::span<const membership::Identity> identities,
                                 PendingMutation& mutation) noexcept;
/**
 * A native release removes the specified peer, never the publisher's own row, never the record's
 * own primary member, and never a member that still holds entity-slot leases. The last two are
 * the fork's `hostRow` and `leaseHeld` refusals: a retract races the departing client's own
 * commit, so a live member's row can only go through `depart_member`.
 * @param refusal Optional. Names which of those two refusals answered, for the route's report.
 */
[[nodiscard]] bool prepare_release(std::uint64_t sessionId,
                                   std::uint64_t memberKey,
                                   PendingMutation& mutation,
                                   ReleaseRefusal* refusal = nullptr) noexcept;
/** Caller serializes native service traffic; consumes the plan on both success and failure. */
[[nodiscard]] bool commit(PendingMutation& mutation) noexcept;
/** Rearms native membership delivery when an admitted player's public dependencies change. */
void invalidate_owner(std::uint64_t accountSoid) noexcept;
} // namespace sunrise::state::activity::reservations
