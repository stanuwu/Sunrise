#include "activity_mission_seed_lease.h"

#include <algorithm>
#include <limits>

#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity/transactions/internal.h"
#include "../../state/build_data/scenarios/definition.h"
#include "encrypted/push/activity/internal.h"
#include "encrypted/push/activity/mission_seed_world_change.h"

namespace sunrise::server::bap {

bool mission_seed_publication_pending(const MissionSeedLease& lease,
                                      std::uint64_t generation) noexcept {
    return lease.configured && lease.bindingGeneration == generation && !lease.regionArrivalPending
           && (lease.revision != lease.publishedRevision
               || (lease.fullSetPublished && lease.revision != lease.fullSetTransportRevision));
}

void commit_mission_seed_publication(Session& session, const RosterPublication& staged) noexcept {
    auto& lease = session.activityMissionSeed;
    if (!staged.staged || !staged.hasMissionSeedRevision || !lease.configured
        || staged.bindingGeneration != session.activity.bindingGeneration
        || lease.bindingGeneration != session.activity.bindingGeneration
        || !staged.missionSeedRevision || lease.revision != staged.missionSeedRevision) {
        return;
    }
    lease.publishedRevision = staged.missionSeedRevision;
    if (staged.missionSeedFullSet) {
        lease.fullSetTransportRevision = staged.missionSeedRevision;
    }
}

bool mission_seed_published(const Session& recipient,
                            const ActivityMissionSeedPlan& plan) noexcept {
    const auto& lease = recipient.activityMissionSeed;
    return lease.configured && lease.scriptSelected && lease.revision != 0
           && lease.bindingGeneration == recipient.activity.bindingGeneration
           && lease.revision == lease.publishedRevision
           && lease.revision == lease.fullSetTransportRevision && !lease.regionArrivalPending
           && same_mission_seed_plan(lease.plan, plan);
}

bool same_mission_seed_plan(const ActivityMissionSeedPlan& left,
                            const ActivityMissionSeedPlan& right) noexcept {
    if (left.omissionCount > left.omissions.size() || left.omissionCount != right.omissionCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.omissionCount; ++index) {
        if (left.omissions[index].objectTag != right.omissions[index].objectTag
            || left.omissions[index].registryKey != right.omissions[index].registryKey) {
            return false;
        }
    }
    return left.activityRow == right.activityRow && left.scenarioRow == right.scenarioRow
           && left.stateRow == right.stateRow && left.bubbleRow == right.bubbleRow
           && left.bubbleOrdinal == right.bubbleOrdinal && left.stateOrdinal == right.stateOrdinal
           && left.entryIndex == right.entryIndex && left.sliceSetIndex == right.sliceSetIndex
           && left.effectiveRegion == right.effectiveRegion
           && left.occurrenceCount == right.occurrenceCount && left.groupCount == right.groupCount
           && left.authMappingSlots == right.authMappingSlots
           && left.authResetSlots == right.authResetSlots
           && left.senseSuppressedSlots == right.senseSuppressedSlots;
}

bool prepare_mission_seed_lease(const MissionSeedLease& current,
                                const ActivityMissionSeedPlan& plan,
                                std::uint64_t generation,
                                std::int32_t heldRegion,
                                MissionSeedLease& output) noexcept {
    output = current;
    if (!generation || plan.omissionCount > plan.omissions.size()) {
        return false;
    }
    if (!output.configured || output.bindingGeneration != generation) {
        output = {};
    }
    if (output.registeredRegionCount > output.registeredRegions.size()
        || (output.configured && output.plan.scenarioRow != plan.scenarioRow)) {
        return false;
    }
    const bool targetHeld = encrypted::push::activity::mission_seed_arrival_window_closed(
        heldRegion, plan.effectiveRegion);
    if (output.configured && same_mission_seed_plan(output.plan, plan)) {
        if (targetHeld) {
            output.regionArrivalPending = false;
        }
        output.scriptSelected = true;
        return true;
    }
    if (output.configured && output.revision == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    const auto regions = std::span(output.registeredRegions).first(output.registeredRegionCount);
    if (std::find(regions.begin(), regions.end(), plan.effectiveRegion) == regions.end()) {
        if (output.registeredRegionCount == output.registeredRegions.size()) {
            return false;
        }
        output.registeredRegions[output.registeredRegionCount++] = plan.effectiveRegion;
    }
    if (output.configured
        && encrypted::push::activity::mission_seed_selection_needs_arrival(
            output.plan.effectiveRegion, plan.effectiveRegion, heldRegion)) {
        // A second selection during an outstanding transition must preserve the world the
        // recipient is still tearing down, rather than the unvisited intermediate selection.
        if (!output.regionArrivalPending) {
            output.previousPlan = output.plan;
        }
        output.regionArrivalPending = true;
    } else if (targetHeld || !output.configured) {
        output.regionArrivalPending = false;
    }
    output.revision = output.configured ? output.revision + 1 : 1;
    output.plan = plan;
    output.bindingGeneration = generation;
    output.configured = true;
    output.scriptSelected = true;
    return true;
}

bool mission_seed_recipient(const Session& session,
                            const state::activity::SessionBinding& binding) noexcept {
    return session.id != 0 && session.authenticated && session.activity.bindingGeneration != 0
           && session.activity.role != ActivityClientRole::none
           && state::activity::same_binding(session.activity.session, binding)
           && state::activity::transactions::same_destination(session.activity.session.destination,
                                                              binding.destination);
}

bool mission_seed_public_link(const Session& link, const Session& recipient) noexcept {
    // A pooled public host keeps the first fireteam's binding as every link's source, so the
    // account is what ties a public link to the private link beside it.
    return link.id != 0 && link.authenticated && link.activity.bindingGeneration != 0
           && link.activity.role == ActivityClientRole::publicTarget
           && link.accountHandle != state::kInvalidAccount
           && link.accountHandle == recipient.accountHandle;
}

bool mission_seed_region_published(const Session& link, std::uint32_t effectiveRegion) noexcept {
    const auto& lease = link.activityMissionSeed;
    return lease.configured && lease.revision != 0
           && lease.bindingGeneration == link.activity.bindingGeneration
           && lease.revision == lease.publishedRevision
           && lease.revision == lease.fullSetTransportRevision && !lease.regionArrivalPending
           && lease.plan.effectiveRegion == effectiveRegion;
}

bool mission_seed_plan_hosted(const Session& session,
                              const ActivityMissionSeedPlan& plan) noexcept {
    const std::uint32_t bubble =
        plan.effectiveRegion / middleware::content::packages::tables::kSliceSetIndexFactor;
    return bubble < state::build_data::scenarios::kBubbleCapacity
           && ((encrypted::push::activity::hosted_bubble_mask(session) >> bubble) & 1U) != 0;
}

bool mission_seed_hosted_publication_locked(const state::activity::SessionBinding& binding,
                                            const ActivityMissionSeedPlan& plan,
                                            bool everyClient) noexcept {
    bool hosting = false;
    for (const Session& recipient : sessions()) {
        if (!mission_seed_recipient(recipient, binding)) {
            continue;
        }
        if (mission_seed_plan_hosted(recipient, plan)) {
            if (!mission_seed_published(recipient, plan)) {
                return false;
            }
            hosting = true;
            continue;
        }
        // This link publishes nothing of a bubble it does not host. The client's public link
        // hosts it and runs the free-roam activity, so it cannot carry this plan: it publishes
        // the bubble's own state, and that receipt is the client's acknowledgement of the
        // bubble's content.
        bool clientHosting = false;
        for (const Session& link : sessions()) {
            if (!mission_seed_public_link(link, recipient)
                || !mission_seed_plan_hosted(link, plan)) {
                continue;
            }
            if (!mission_seed_region_published(link, plan.effectiveRegion)) {
                return false;
            }
            clientHosting = true;
        }
        if (!clientHosting) {
            if (everyClient) {
                return false;
            }
            continue;
        }
        hosting = true;
    }
    return hosting || !everyClient;
}

bool inherit_mission_seed_locked(Session& session, std::int32_t heldRegion) noexcept {
    if (!mission_seed_recipient(session, session.activity.session)) {
        return false;
    }
    const auto& lease = session.activityMissionSeed;
    if (lease.configured && lease.bindingGeneration == session.activity.bindingGeneration
        && lease.scriptSelected) {
        return true;
    }
    ActivityMissionSeedPlan plan{};
    bool selected{};
    if (!selected_mission_seed_locked(session.activity.session, plan, selected)) {
        return false;
    }
    if (!selected) {
        return true;
    }
    MissionSeedLease next{};
    if (!prepare_mission_seed_lease(
            lease, plan, session.activity.bindingGeneration, heldRegion, next)) {
        return false;
    }
    session.activityMissionSeed = next;
    return true;
}

bool selected_mission_seed_locked(const state::activity::SessionBinding& binding,
                                  ActivityMissionSeedPlan& plan,
                                  bool& selected) noexcept {
    plan = {};
    selected = false;
    const MissionSeedLease* current = nullptr;
    for (const auto& peer : sessions()) {
        const auto& candidate = peer.activityMissionSeed;
        if (!mission_seed_recipient(peer, binding) || !candidate.configured
            || !candidate.scriptSelected
            || candidate.bindingGeneration != peer.activity.bindingGeneration) {
            continue;
        }
        if (current != nullptr && !same_mission_seed_plan(current->plan, candidate.plan)) {
            return false;
        }
        current = &candidate;
    }
    if (current != nullptr) {
        plan = current->plan;
        selected = true;
    }
    return true;
}

/** Validates and clears stale state on one already-selected ActivityClient session. */
ActivityMissionSeedLeaseStatus mission_seed_session_status(
    Session& session, std::uint32_t scenarioRow, std::uint64_t expectedGeneration) noexcept {
    if (expectedGeneration == 0 || session.activity.bindingGeneration != expectedGeneration) {
        return ActivityMissionSeedLeaseStatus::staleActivityClient;
    }
    MissionSeedLease& lease = session.activityMissionSeed;
    if (lease.configured && lease.bindingGeneration != session.activity.bindingGeneration) {
        lease = {};
    }
    if (lease.configured && lease.plan.scenarioRow != scenarioRow) {
        return ActivityMissionSeedLeaseStatus::wrongScenario;
    }
    return ActivityMissionSeedLeaseStatus::ready;
}

/** Copies one validated session lease into the public read-only view. */
void read_mission_seed_lease(const Session& session,
                             std::size_t matchingLinks,
                             ActivityMissionSeedLeaseView& output) noexcept {
    const MissionSeedLease& lease = session.activityMissionSeed;
    output = {};
    output.plan = lease.configured ? lease.plan : ActivityMissionSeedPlan{};
    output.matchingLinks = matchingLinks;
    output.activityClientGeneration = session.activity.bindingGeneration;
    output.revision = lease.revision;
    // The view is this link's own receipt: a roster of its own committed since the selection,
    // whether or not it hosts the plan's bubble. A state-local output needs no more than that; the
    // output's pending body installs the state-local group it targets. Whether the bubble's own
    // entity definitions reached the clients is mission_seed_hosted_publication_locked's question,
    // asked by the output gates.
    output.publishedRevision = lease.publishedRevision;
    output.configured = lease.configured;
    output.publicationPending = lease.configured && lease.revision != output.publishedRevision;
    output.regionArrivalPending = lease.configured && lease.regionArrivalPending;
}

} // namespace sunrise::server::bap
