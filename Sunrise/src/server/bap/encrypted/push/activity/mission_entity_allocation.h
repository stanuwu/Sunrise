#pragma once

#include "../../../../../state/activity/member_departure.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

/** A complete new definition set or squad placement can allocate many native objects together. */
[[nodiscard]] inline bool mission_entity_allocation_owed(const Session& session,
                                                         bool squadPlacement) noexcept {
    const auto& seed = session.activityMissionSeed;
    return squadPlacement
           || (seed.configured && seed.bindingGeneration == session.activity.bindingGeneration
               && seed.revision != 0 && seed.fullSetPublished
               && seed.fullSetTransportRevision != seed.revision);
}

/**
 * Reserve only unused indices in this authenticated member's existing allocation domain.
 * Allocated objects and the client's retained spare indices are already held and are excluded.
 * The native recipient may return any surplus through its ordinary type-21 message.
 */
[[nodiscard]] inline bool
prepare_mission_entity_allocation(const Session& session,
                                  state::activity::entity_slots::PendingMutation& output) noexcept {
    namespace slots = state::activity::entity_slots;
    output = {};
    if (!session.authenticated || !session.activity.bindingGeneration || !session.activityMemberKey
        || !state::activity::binding_matches(session.activity.session)
        || !slots::prepare_grant(session.activity.session.sessionId, slots::kSlotCount, output)) {
        return false;
    }
    if (output.shared && output.identity.memberKey != session.activityMemberKey) {
        output = {};
        return false;
    }
    if (!slots::slot_count(output.mask)) {
        output = {};
    }
    return true;
}

/**
 * Last fallible operation before copying an unsolicited roster frame to its transport caller.
 * Solicited reports defer this publication so their own State transaction cannot invalidate the
 * allocation revision. A stale allocation sends neither its IDs nor the dependent roster.
 */
[[nodiscard]] inline bool commit_mission_entity_allocation(Session& session) noexcept {
    auto& staged = session.activityRosterStaged;
    if (!staged.staged) {
        return true;
    }
    if (!staged.entitySlotAllocation.prepared) {
        return !staged.entityRetirement.pending
               || state::activity::advance_replication_sequence(session.activity.session,
                                                                staged.retirementSequence);
    }
    staged.entitySlotAllocation.advanceReplicationSequence = staged.entityRetirement.pending;
    staged.entitySlotAllocation.expectedReplicationSequence = staged.retirementSequence;
    return staged.bindingGeneration == session.activity.bindingGeneration
           && state::activity::binding_matches(session.activity.session)
           && state::activity::entity_slots::commit(staged.entitySlotAllocation);
}

/**
 * A keepalive may commit its own membership revision before publishing its roster.
 * Refresh only that revision dependency; every allocated bit, owner and member row must agree.
 */
[[nodiscard]] inline bool refresh_mission_entity_allocation(Session& session) noexcept {
    auto& staged = session.activityRosterStaged;
    auto& previous = staged.entitySlotAllocation;
    if (!staged.staged || !previous.prepared) {
        return true;
    }
    state::activity::entity_slots::PendingMutation current{};
    if (staged.bindingGeneration != session.activity.bindingGeneration
        || !prepare_mission_entity_allocation(session, current) || !current.prepared
        || current.mask != previous.mask || current.identity != previous.identity
        || current.shared != previous.shared || current.memberRow != previous.memberRow
        || current.targetSlot != previous.targetSlot) {
        return false;
    }
    previous = current;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::activity
