#pragma once

#include <cstddef>
#include <cstdint>

#include "definition.h"

namespace sunrise::state::gameplay::physics {

/** @return True when the logical index is present in the lease mask. */
[[nodiscard]] bool mask_has(const activity::entity_slots::LeaseMask& mask,
                            std::size_t index) noexcept;

/** @return True when no logical index belongs to both activity leases. */
[[nodiscard]] bool masks_are_disjoint(const ActivityReplicaContext& context) noexcept;

/** @return True when the role names a publishable activity context. */
[[nodiscard]] bool valid_role(ReplicaRole role) noexcept;

/** @return True when every authenticated peer generation field matches. */
[[nodiscard]] bool equal_peer_key(const PeerKey& left, const PeerKey& right) noexcept;

/** @return True when the peer, group, and view epoch match. */
[[nodiscard]] bool equal_view_key(const ViewKey& left, const ViewKey& right) noexcept;

/** @return True when every required peer and view identity field is present. */
[[nodiscard]] bool valid_view_key(const ViewKey& view) noexcept;

/** @return True when the reference kind is owned outside peer state. */
[[nodiscard]] bool valid_host_reference_kind(ReferenceKind kind) noexcept;

/** @return True when the contribution kind owns a packet or replay reference. */
[[nodiscard]] bool valid_contribution_kind(ContributionKind kind) noexcept;

/** @return The aggregate reference kind owned by the contribution. */
[[nodiscard]] ReferenceKind contribution_reference_kind(ContributionKind kind) noexcept;

/** @return The fixed aggregate-counter index for the reference kind. */
[[nodiscard]] std::size_t reference_index(ReferenceKind kind) noexcept;

/** @return True when both handles name one exact context generation. */
[[nodiscard]] bool equal_context_handle(ContextHandle left, ContextHandle right) noexcept;

/** @return True when both views have the same peer and group owner. */
[[nodiscard]] bool equal_view_owner(const ViewKey& left, const ViewKey& right) noexcept;

/** @return True when every internal allocation identity field matches. */
[[nodiscard]] bool equal_allocation(const Allocation& left, const Allocation& right) noexcept;

} // namespace sunrise::state::gameplay::physics
