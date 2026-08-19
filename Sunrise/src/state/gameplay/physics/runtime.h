#pragma once

#include <cstddef>
#include <cstdint>

#include "definition.h"

namespace sunrise::state::gameplay::physics {

/** Clears fixed storage and makes the dormant replica state available. */
void initialize() noexcept;

/** Clears every context and invalidates all outstanding handles. */
void shutdown() noexcept;

/**
 * Publishes one checked activity and initializes its lease owners.
 * @param context Activity identity and disjoint client/server masks.
 * @param handle Receives the generation-checked fixed slot.
 * @return True when a free context slot accepted the whole snapshot.
 */
[[nodiscard]] bool publish_context(const ActivityReplicaContext& context,
                                   ContextHandle& handle) noexcept;

/**
 * Finds an exact role/session pair without treating equal numeric ids as aliases.
 * @param role Required activity role.
 * @param activitySessionId Required nonzero activity id.
 * @param handle Receives the generation-checked fixed slot.
 * @return True when the exact context is present.
 */
[[nodiscard]] bool
find_context(ReplicaRole role, std::uint64_t activitySessionId, ContextHandle& handle) noexcept;

/**
 * Copies one activity context without leaking State-owned pointers.
 * @param handle Exact context handle.
 * @param context Receives the stored activity snapshot.
 * @return True when the handle still names the same context generation.
 */
[[nodiscard]] bool snapshot_context(ContextHandle handle, ActivityReplicaContext& context) noexcept;

/**
 * Copies the activity-wide reuse epoch.
 * @param handle Exact context handle.
 * @param reuseEpoch Receives the current nonzero epoch.
 * @return True when the context exists.
 */
[[nodiscard]] bool snapshot_reuse_epoch(ContextHandle handle, std::uint64_t& reuseEpoch) noexcept;

/**
 * Copies one allocator slot without exposing fixed State storage.
 * @param handle Exact context handle.
 * @param index Entity index in the 13-bit space.
 * @param snapshot Receives the slot state.
 * @return True when the context and index are valid.
 */
[[nodiscard]] bool
snapshot_slot(ContextHandle handle, std::size_t index, SlotSnapshot& snapshot) noexcept;

/**
 * Tears down one activity context and invalidates its allocations and peer replicas.
 * @param handle Exact context handle.
 * @return True when the context was cleared.
 */
[[nodiscard]] bool reset_context(ContextHandle handle) noexcept;

/**
 * Resets wire incarnation budgets after the whole activity has drained.
 * @param handle Exact context handle.
 * @return True when no actor, peer, ticket, or reference can name the old epoch.
 */
[[nodiscard]] bool advance_reuse_epoch(ContextHandle handle) noexcept;

/** @param oldSequence Stored raw byte. @return Exact next reservation byte. */
[[nodiscard]] std::uint8_t next_allocation_sequence(std::uint8_t oldSequence) noexcept;

/**
 * Reserves the first available server-owned index for one actor.
 * @param handle Exact activity context.
 * @param actorId Nonzero internal actor id, unique in the context.
 * @param allocation Receives the wire and internal allocation identity.
 * @return True when one server-reserved index was advanced and reserved.
 */
[[nodiscard]] bool
reserve(ContextHandle handle, std::uint64_t actorId, Allocation& allocation) noexcept;

/**
 * Marks post-reservation preparation failure without making the identity reusable.
 * @param allocation Exact reserved allocation.
 * @return True when the slot entered abort quarantine.
 */
[[nodiscard]] bool abort_reservation(const Allocation& allocation) noexcept;

/**
 * Advances a reserved allocation to create-queued.
 * @param allocation Exact active allocation.
 * @return True when the lifecycle transition applied.
 */
[[nodiscard]] bool queue_create(const Allocation& allocation) noexcept;

/**
 * Commits a create-queued allocation as live.
 * @param allocation Exact active allocation.
 * @return True when the lifecycle transition applied.
 */
[[nodiscard]] bool commit_create(const Allocation& allocation) noexcept;

/**
 * Advances a created allocation to remove-queued.
 * @param allocation Exact active allocation.
 * @return True when the lifecycle transition applied.
 */
[[nodiscard]] bool queue_remove(const Allocation& allocation) noexcept;

/**
 * Retires a remove-queued actor and reclaims it only after all references drain.
 * @param allocation Exact active allocation.
 * @return True when retirement applied, including quarantine.
 */
[[nodiscard]] bool settle_remove(const Allocation& allocation) noexcept;

/**
 * Publishes one peer-owned view under an exact activity context.
 * @param context Exact activity context.
 * @param view Exact authenticated peer, group, and view identity.
 * @param replica Receives the generation-checked peer replica.
 * @return True when a free peer slot accepted the view.
 */
[[nodiscard]] bool publish_peer_replica(ContextHandle context,
                                        const ViewKey& view,
                                        PeerReplicaHandle& replica) noexcept;

/**
 * Copies one peer replica without exposing State-owned pointers.
 * @param replica Exact peer replica handle.
 * @param snapshot Receives its view and owner counts.
 * @return True when the peer replica exists.
 */
[[nodiscard]] bool snapshot_peer_replica(PeerReplicaHandle replica,
                                         PeerReplicaSnapshot& snapshot) noexcept;

/**
 * Replaces one peer's view within the same group and releases only its old references.
 * @param replica Exact old peer replica.
 * @param view New view for the same authenticated peer and group owner.
 * @param replacement Receives a new handle, or the same handle for an identical view.
 * @return True when the peer replica owns the requested view.
 */
[[nodiscard]] bool replace_peer_view(PeerReplicaHandle replica,
                                     const ViewKey& view,
                                     PeerReplicaHandle& replacement) noexcept;

/**
 * Retires one peer replica and releases only its owned references and tickets.
 * @param replica Exact peer replica handle.
 * @return True when the replica was cleared.
 */
[[nodiscard]] bool retire_peer_replica(PeerReplicaHandle replica) noexcept;

/**
 * Retains one allocation in one peer's baseline or relevance state.
 * @param replica Exact peer replica.
 * @param allocation Exact active allocation.
 * @return True when a new peer-owned reference was added.
 */
[[nodiscard]] bool retain_for_peer(PeerReplicaHandle replica,
                                   const Allocation& allocation) noexcept;

/**
 * Releases one allocation from one peer's baseline or relevance state.
 * @param replica Exact peer replica.
 * @param allocation Exact retained allocation.
 * @return True when that peer's exact reference was released.
 */
[[nodiscard]] bool release_for_peer(PeerReplicaHandle replica,
                                    const Allocation& allocation) noexcept;

/**
 * Opens one exact packet or replay-horizon reference owned by a peer replica.
 * @param replica Exact peer replica which already retains the allocation.
 * @param allocation Exact active allocation.
 * @param kind Packet or replay owner kind.
 * @param ticket Receives the single-use ticket handle.
 * @return True when a free ticket slot and counter were committed together.
 */
[[nodiscard]] bool open_contribution_ticket(PeerReplicaHandle replica,
                                            const Allocation& allocation,
                                            ContributionKind kind,
                                            ContributionTicket& ticket) noexcept;

/**
 * Releases the exact reference owned by one contribution ticket.
 * @param ticket Exact single-use ticket.
 * @return True when the ticket was live and settled once.
 */
[[nodiscard]] bool settle_contribution_ticket(ContributionTicket ticket) noexcept;

/**
 * Opens one exact dependency or command reference outside peer state.
 * @param allocation Exact active allocation.
 * @param kind Dependency or command owner kind.
 * @param ticket Receives the single-use generation-checked owner.
 * @return True when a free ticket was committed.
 */
[[nodiscard]] bool open_host_reference_ticket(const Allocation& allocation,
                                              ReferenceKind kind,
                                              HostReferenceTicket& ticket) noexcept;

/**
 * Releases one exact dependency or command reference.
 * @param ticket Exact single-use host-reference ticket.
 * @return True when the ticket was live and settled once.
 */
[[nodiscard]] bool settle_host_reference_ticket(HostReferenceTicket ticket) noexcept;

} // namespace sunrise::state::gameplay::physics
