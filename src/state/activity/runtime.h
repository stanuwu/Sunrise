#pragma once

#include <cstdint>

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
 * Commits one prepared activity-session allocation when its revisions still match.
 * @param allocation Prepared plan. Always cleared before this function returns.
 * @return True when the allocation committed in one step.
 */
[[nodiscard]] bool commit(PendingAllocation& allocation) noexcept;

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

/** How far the client has got through the current destination load. */
enum class WorldPhase : std::uint8_t {
    /** No destination load is running. Orbit sits here, and the spawn is never held. */
    idle,
    /** The step that arms the black fade has started and the in-world step has not been reached. */
    transitioning,
    /** The in-world step is entered, so the fade is armed and a spawn now releases it. */
    arrived,
};

/**
 * Records how far the current destination load has got.
 * Entering transitioning from any other phase resets the load's start tick.
 * @param phase Phase the client's own boot-flow step maps to.
 */
void note_world_phase(WorldPhase phase) noexcept;

/** @return How far the client has got through the current destination load. */
[[nodiscard]] WorldPhase world_phase() noexcept;

/** @return Milliseconds since the running load started, or zero when none is running. */
[[nodiscard]] std::uint64_t world_transition_age() noexcept;

} // namespace sunrise::state::activity
