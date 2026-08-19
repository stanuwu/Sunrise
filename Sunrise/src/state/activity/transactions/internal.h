#pragma once

#include <cstddef>

#include "../definition.h"

namespace sunrise::state::activity::transactions {

/** @return Matching slot, or the fixed-table absent value when the session is not there. */
inline std::size_t find_session(const ActivityState& state, std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < state.sessions.size(); ++index) {
        const SessionRecord& record = state.sessions[index];
        if (record.occupied && record.sessionId == sessionId) {
            return index;
        }
    }
    return kInvalidSessionSlot;
}

/**
 * Picks the first empty record, or the oldest unretained occupied one.
 * @param state Activity State, guarded by the root State lock.
 * @return Target slot, or the invalid-slot sentinel when every occupied row is retained.
 */
inline std::size_t select_target(const ActivityState& state) noexcept {
    for (std::size_t index = 0; index < state.sessions.size(); ++index) {
        if (!state.sessions[index].occupied) {
            return index;
        }
    }

    std::size_t selected = kInvalidSessionSlot;
    for (std::size_t index = 0; index < state.sessions.size(); ++index) {
        if (state.sessions[index].bindingRetainCount != 0) {
            continue;
        }
        // Strict comparison keeps the lowest slot when creation revisions tie.
        if (selected == kInvalidSessionSlot
            || state.sessions[index].createdRevision < state.sessions[selected].createdRevision) {
            selected = index;
        }
    }
    return selected;
}

/** Activity-session class the published soid carries between the account half and the index. */
inline constexpr std::uint64_t kSessionClass = 0x00200000ULL;

/**
 * Builds the soid the Client keys its own activity-session record by.
 * A bare counter matches no record, and sim event 20 then drops the roster with no line. The
 * built form is `0x9EAA300100200001`.
 * @param soidBase Account half, or zero while no account is loaded.
 * @param counter Allocator index for this record.
 * @return Published session soid.
 */
[[nodiscard]] inline std::uint64_t compose_session_soid(std::uint64_t soidBase,
                                                        std::uint64_t counter) noexcept {
    return soidBase != 0 ? soidBase | kSessionClass | counter : counter;
}

/** Account half of every soid this account owns, which the activity session shares. */
inline constexpr std::uint64_t kAccountMask = 0xFFFFFFFF00000000ULL;

/**
 * Recovers the allocator counter from a published session soid.
 * The check is a round trip rather than a bit test, so an id this allocator could not have built
 * is refused whatever shape it has.
 * @param soidBase Account half, or zero while no account is loaded.
 * @param sessionId Published session soid.
 * @param counter Cleared, then receives the allocator index.
 * @return True when the soid is one this allocator publishes.
 */
[[nodiscard]] inline bool decompose_session_soid(std::uint64_t soidBase,
                                                 std::uint64_t sessionId,
                                                 std::uint64_t& counter) noexcept {
    counter = 0;
    if (soidBase == 0) {
        counter = sessionId;
        return counter != kAbsentSessionId;
    }
    counter = sessionId & ~(kAccountMask | kSessionClass);
    return counter != kAbsentSessionId && compose_session_soid(soidBase, counter) == sessionId;
}

/**
 * Tests whether the activity allocator can commit one more id.
 * @param state Activity allocator to check.
 * @return True when one more id can commit without wrapping a counter.
 */
inline bool allocation_available(const ActivityState& state) noexcept {
    return !state.allocatorExhausted && state.nextSessionId != kAbsentSessionId
           && state.stateRevision != kMaximumRevision
           && state.allocatorRevision != kMaximumRevision;
}

/**
 * Advances the allocator without wrapping the id or its revision.
 * @param state Activity State held under the root State write lock.
 */
inline void advance_allocator(ActivityState& state) noexcept {
    if (state.nextSessionId == kMaximumSessionId) {
        // The highest id stays committed, but there is no next one.
        state.allocatorExhausted = true;
    } else {
        ++state.nextSessionId;
    }
    ++state.allocatorRevision;
    if (state.allocatorRevision == kMaximumRevision) {
        state.allocatorExhausted = true;
    }
}

} // namespace sunrise::state::activity::transactions
