#include <Windows.h>

#include "../../account/account_context.h"
#include "../../runtime/storage/internal.h"
#include "../destination/activity_destination_validation.h"
#include "../shared_allocation.h"
#include "internal.h"
#include "shared_allocation_policy.h"

namespace sunrise::state::activity {
namespace {
bool prepare_locked(const ActivityState& state,
                    const destination::DestinationSelection& selection,
                    const LaunchParty& party,
                    std::uint64_t& sessionId,
                    PendingAllocation& allocation) noexcept {
    if (!destination::valid(selection) || state.stateRevision == kMaximumRevision
        || party.publisherAccount != account_primary_soid(bound_account())) {
        return false;
    }
    PendingAllocation plan{};
    if (!transactions::capture_launch_party(party, plan.launchProfileGuard)) {
        return false;
    }
    std::size_t shared = kInvalidSessionSlot;
    for (std::size_t i = 0; i < state.sessions.size(); ++i) {
        const auto& candidate = state.sessions[i];
        if (transactions::same_launch_destination(candidate.destination, selection)
            && transactions::share_launch(state, candidate, party)
            && (shared == kInvalidSessionSlot
                || candidate.createdRevision > state.sessions[shared].createdRevision)) {
            shared = i;
        }
    }
    plan.shared = true;
    plan.launchParty = plan.launchPartyGuard = party;
    plan.soidBase = party.publisherAccount & transactions::kAccountMask;
    plan.expectedStateRevision = state.stateRevision;
    plan.expectedAllocatorRevision = state.allocatorRevision;
    plan.expectedNextSessionId = state.nextSessionId;
    plan.expectedFireteamRevision = state.fireteams.revision();
    if (shared != kInvalidSessionSlot) {
        const auto& record = state.sessions[shared];
        plan.destination = record.destination;
        plan.sessionId = record.sessionId;
        plan.targetSlot = shared;
        plan.expectedRecordRevision = record.recordRevision;
        plan.expectedCreatedRevision = record.createdRevision;
        plan.reused = true;
    } else {
        if (!transactions::allocation_available(state)) {
            return false;
        }
        plan.targetSlot = transactions::select_target(state);
        if (plan.targetSlot == kInvalidSessionSlot) {
            return false;
        }
        plan.destination = selection;
        plan.sessionId = transactions::compose_session_soid(plan.soidBase, state.nextSessionId);
    }
    plan.prepared = true;
    sessionId = plan.sessionId;
    allocation = plan;
    return true;
}
} // namespace

bool prepare_shared_session(const destination::DestinationSelection& selection,
                            const LaunchParty& party,
                            std::uint64_t& sessionId,
                            PendingAllocation& allocation) noexcept {
    sessionId = 0;
    allocation = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const bool ready =
        prepare_locked(runtime::storage::g_state.activity, selection, party, sessionId, allocation);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return ready;
}

bool prepare_shared_session(const LaunchParty& party,
                            std::uint64_t& sessionId,
                            PendingAllocation& allocation) noexcept {
    sessionId = 0;
    allocation = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& state = runtime::storage::g_state.activity;
    const bool ready = prepare_locked(
        state, state.defaults.defaultDestination.selection, party, sessionId, allocation);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return ready;
}

namespace transactions {
bool commit_shared_allocation(const PendingAllocation& plan) noexcept {
    if (!plan.prepared || !plan.shared || plan.recreated || !plan.sessionId
        || plan.targetSlot >= kSessionCapacity || !destination::valid(plan.destination)
        || plan.launchParty != plan.launchPartyGuard
        || plan.launchParty.publisherAccount != account_primary_soid(bound_account())
        || plan.soidBase != (plan.launchParty.publisherAccount & kAccountMask)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& state = runtime::storage::g_state.activity;
    LaunchPartyGuard current;
    bool ready =
        state.stateRevision != kMaximumRevision && state.stateRevision == plan.expectedStateRevision
        && state.allocatorRevision == plan.expectedAllocatorRevision
        && state.nextSessionId == plan.expectedNextSessionId
        && state.fireteams.revision() == plan.expectedFireteamRevision
        && capture_launch_party(plan.launchParty, current) && current == plan.launchProfileGuard;
    auto& record = state.sessions[plan.targetSlot];
    if (ready && plan.reused) {
        ready = record.sessionId == plan.sessionId
                && record.createdRevision == plan.expectedCreatedRevision
                && record.recordRevision == plan.expectedRecordRevision
                && same_destination(record.destination, plan.destination)
                && share_launch(state, record, plan.launchParty);
        if (ready) {
            auto& owner = record.launchOwners[launch_owner_slot(record, plan.launchParty)];
            const LaunchOwner incoming{plan.launchParty.publisherAccount,
                                       plan.launchParty.publisherCharacter};
            if (owner != incoming) {
                owner = incoming;
                record.recordRevision = ++state.stateRevision;
            }
        }
    } else if (ready) {
        ready = allocation_available(state) && select_target(state) == plan.targetSlot
                && !plan.expectedCreatedRevision && !plan.expectedRecordRevision
                && plan.sessionId == compose_session_soid(plan.soidBase, state.nextSessionId);
        if (ready) {
            record = {};
            record.destination = plan.destination;
            record.sessionId = plan.sessionId;
            record.createdRevision = record.recordRevision = ++state.stateRevision;
            record.timeOrigin = GetTickCount64() / 1'000;
            record.launchParty = plan.launchParty;
            record.launchOwners[0] = {plan.launchParty.publisherAccount,
                                      plan.launchParty.publisherCharacter};
            record.occupied = true;
            advance_allocator(state);
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ready;
}
} // namespace transactions
} // namespace sunrise::state::activity
