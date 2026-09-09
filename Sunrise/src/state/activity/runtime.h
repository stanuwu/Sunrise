#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"
#include "entity_slots/runtime.h"

namespace sunrise::state::activity {

/**
 * Prepares one allocation with State's fixed default destination, without changing State.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the picked record and allocator revisions can be committed.
 */
[[nodiscard]] bool prepare_session(std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Prepares one allocation with an explicit checked scalar destination.
 * @param selection Caller-owned destination, copied into the read-only allocation plan.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the destination and allocator snapshot can be committed together.
 */
[[nodiscard]] bool prepare_session(const destination::DestinationSelection& selection,
                                   std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Prepares recreation of an earlier id at the authored default destination.
 * The client reads its activity-client id once, so a record it still names must come back under
 * that id. TODO: no caller yet. Reconnect has to reach this before it is called.
 * @param sessionId Published session soid the client still names.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the id is one this allocator published and no record holds it now.
 */
[[nodiscard]] bool prepare_session_with_id(std::uint64_t sessionId,
                                           PendingAllocation& allocation) noexcept;

/**
 * Prepares recreation of an earlier id with an explicit checked destination.
 * TODO: no caller yet. Reconnect has to reach this before it is called.
 * @param selection Destination that must commit with the recreated session id.
 * @param sessionId Published session soid this allocator previously issued.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the id is recreatable and one unretained record can hold it.
 */
[[nodiscard]] bool prepare_session_with_id(const destination::DestinationSelection& selection,
                                           std::uint64_t sessionId,
                                           PendingAllocation& allocation) noexcept;

/**
 * Commits one prepared activity-session allocation when its revisions still match.
 * @param allocation Prepared plan. Always cleared before this function returns.
 * @return True when the allocation committed in one step.
 */
[[nodiscard]] bool commit(PendingAllocation& allocation) noexcept;

/**
 * Frees one committed activity-session record.
 * Nothing else clears one. A host that allocates per region must release them, or the table
 * fills and the oldest record is evicted.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when an unretained record held that id and is now free.
 */
bool release_session(std::uint64_t sessionId) noexcept;

/**
 * Tests whether a nonzero activity session id is still in the fixed-size table.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the id is in a committed record.
 */
[[nodiscard]] bool contains(std::uint64_t sessionId) noexcept;

/**
 * Tests whether a committed activity session id has finished a join.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the current record has a committed join revision.
 */
[[nodiscard]] bool is_joined(std::uint64_t sessionId) noexcept;

/**
 * Copies the exact destination and generation of one committed session.
 * @param sessionId Committed nonzero activity-session id.
 * @param output Cleared, then receives the immutable binding identity.
 * @return True when the session still has a committed record.
 */
[[nodiscard]] bool snapshot_binding(std::uint64_t sessionId, SessionBinding& output) noexcept;

/**
 * Copies the binding of every committed session.
 * A per-session service needs the whole set, including private activities that have no gameplay
 * peer from which to discover their identity.
 * @param output Caller-owned storage, which bounds how many are copied.
 * @param count Receives the number written.
 * @return True when every committed session fit the storage.
 */
[[nodiscard]] bool snapshot_sessions(std::span<SessionBinding> output, std::size_t& count) noexcept;

/** One committed session's identity and join state, without its mission or membership body. */
struct SessionRosterRow final {
    SessionBinding binding{};
    std::uint64_t memberKey{};
    /** The identity message 12 publishes at member record `+16`, or zero before one. */
    std::uint64_t joinIdentity{};
    std::uint64_t joinedRevision{};
    bool joined{};
};

/**
 * Copies the join state of every committed session under one read lock.
 * It never exposes a record's mission or membership body.
 * @param output Caller-owned storage, which bounds how many are copied.
 * @param count Receives the number written.
 * @return True when every committed session fit the storage.
 */
[[nodiscard]] bool snapshot_session_roster(std::span<SessionRosterRow> output,
                                           std::size_t& count) noexcept;

/**
 * Copies only exact generations currently retained by a live owner.
 * Cached unretained rows remain available for allocator reuse but are not live host instances.
 * @param output Caller-owned storage.
 * @param count Receives the number written.
 * @return True when every retained generation fit.
 */
[[nodiscard]] bool snapshot_retained_bindings(std::span<SessionBinding> output,
                                              std::size_t& count) noexcept;

/** @return True when the exact bound record generation is still committed. */
[[nodiscard]] bool binding_matches(const SessionBinding& binding) noexcept;

/**
 * Retains an exact record generation against explicit release and allocator eviction.
 * @return True when the binding still matches and its retain count can advance.
 */
[[nodiscard]] bool retain_binding(const SessionBinding& binding) noexcept;

/** Releases one retain only when the exact record generation still matches. */
void release_binding(const SessionBinding& binding) noexcept;

} // namespace sunrise::state::activity
