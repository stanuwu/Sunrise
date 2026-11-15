#include "activity_mission_seed_lease.h"

#include <algorithm>
#include <limits>

#include "../../core/logging/log.h"
#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity/bubble_authority/runtime.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/activity/runtime.h"
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

void commit_owner_seed_publication(Session& link, const RosterPublication& staged) noexcept {
    MissionSeedLease& lease = link.activityPublicOwner.seed;
    if (!staged.staged || !staged.hasOwnerSeedRevision || !lease.configured
        || staged.bindingGeneration != link.activity.bindingGeneration
        || lease.bindingGeneration != link.activity.bindingGeneration
        || !state::activity::same_binding(link.activityPublicOwner.binding, staged.ownerBinding)
        || !staged.ownerSeedRevision || lease.revision != staged.ownerSeedRevision) {
        return;
    }
    lease.publishedRevision = staged.ownerSeedRevision;
    if (staged.ownerSeedFullSet) {
        lease.fullSetTransportRevision = staged.ownerSeedRevision;
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
        // A release requested before this client's first selection still belongs to it.
        const BubbleHandoff handoff = output.bubbleHandoff;
        output = {};
        if (handoff.active(generation)) {
            output.bubbleHandoff = handoff;
        }
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

ActivityMissionSeedLeaseStatus
prepare_bubble_handoff(Session& recipient,
                       const ActivityMissionSeedPlan& plan,
                       const state::activity::membership::ClientPlacement& placement) noexcept {
    namespace authority = state::activity::bubble_authority;
    const std::uint64_t generation = recipient.activity.bindingGeneration;
    MissionSeedLease& lease = recipient.activityMissionSeed;
    BubbleHandoff& handoff = lease.bubbleHandoff;
    // The selection the release was for was replaced, or abandoned for the one already held.
    if (handoff.active(generation) && handoff.destination != plan.effectiveRegion) {
        handoff.retarget(plan.effectiveRegion);
    }
    if (lease.configured && lease.bindingGeneration == generation
        && same_mission_seed_plan(lease.plan, plan)) {
        return ActivityMissionSeedLeaseStatus::ready;
    }
    if (handoff.active(generation)) {
        return handoff.stage == BubbleHandoff::Stage::acknowledged
                   ? ActivityMissionSeedLeaseStatus::ready
                   : ActivityMissionSeedLeaseStatus::outputBusy;
    }
    // Only a client resident in a sibling state of the selected one holds the bubble it reloads.
    // A public target publishes its owner's bubble and never simulates it.
    const std::int32_t held = state::activity::membership::instantiated_region(placement);
    if (recipient.activity.role != ActivityClientRole::privateCurrent || held < 0
        || placement.currentRegion != held
        || static_cast<std::uint32_t>(held) == plan.effectiveRegion
        || (static_cast<std::uint32_t>(held) >> authority::kSliceSetToBubbleShift)
               != (plan.effectiveRegion >> authority::kSliceSetToBubbleShift)) {
        return ActivityMissionSeedLeaseStatus::ready;
    }
    authority::Grant grant{};
    if (!authority::held_grant(
            recipient.activity.session.sessionId,
            generation,
            static_cast<std::uint8_t>(plan.effectiveRegion >> authority::kSliceSetToBubbleShift),
            grant)) {
        return ActivityMissionSeedLeaseStatus::ready;
    }
    handoff = {.generation = generation,
               .source = static_cast<std::uint32_t>(held),
               .destination = plan.effectiveRegion,
               .token = grant.token,
               .stage = BubbleHandoff::Stage::requested};
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=activity stage=bubble_handoff result=requested bubble=%u source=%u "
                      "destination=%u token=%u",
                      static_cast<unsigned>(grant.bubble),
                      static_cast<unsigned>(held),
                      static_cast<unsigned>(plan.effectiveRegion),
                      static_cast<unsigned>(grant.token));
    return ActivityMissionSeedLeaseStatus::outputBusy;
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

std::uint64_t advertised_bubble(const Session& link) noexcept {
    const std::int32_t region = link.activity.advertisedRegion;
    if (region < 0) {
        return 0;
    }
    const std::uint32_t bubble = static_cast<std::uint32_t>(region)
                                 / middleware::content::packages::tables::kSliceSetIndexFactor;
    return bubble < state::build_data::scenarios::kBubbleCapacity ? std::uint64_t{1} << bubble : 0;
}

std::uint64_t owner_public_bubbles(const Session& link, const Session& owner) noexcept {
    return advertised_bubble(link) & ~encrypted::push::activity::hosted_bubble_mask(owner);
}

Session* public_mission_owner_locked(const Session& link) noexcept {
    if (link.activity.role != ActivityClientRole::publicTarget) {
        return nullptr;
    }
    Session* owner = nullptr;
    for (Session& candidate : sessions()) {
        if (candidate.id == 0 || !candidate.authenticated
            || candidate.activity.role != ActivityClientRole::privateCurrent
            || candidate.activity.bindingGeneration == 0
            || candidate.activityJoinGeneration != candidate.activity.bindingGeneration
            || !state::activity::binding_matches(candidate.activity.session)
            || !mission_seed_public_link(link, candidate)) {
            continue;
        }
        if (owner != nullptr) {
            return nullptr;
        }
        owner = &candidate;
    }
    return owner;
}

namespace {

/** @return The bubble a state-local target's region belongs to, or the capacity when none. */
[[nodiscard]] std::uint32_t target_bubble(const activity::host::ScriptableTarget& target) noexcept {
    if (!target.stateLocalRoster || target.stateLocalRegion < 0) {
        return state::build_data::scenarios::kBubbleCapacity;
    }
    return static_cast<std::uint32_t>(target.stateLocalRegion)
           / middleware::content::packages::tables::kSliceSetIndexFactor;
}

/** @return True when the state that owns this region of the owner's mission is public. */
[[nodiscard]] bool owner_state_public(const Session& owner, std::int32_t region) noexcept {
    const state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    state::activity_sdk::BoundView view{};
    const state::activity_sdk::Selection selection{
        owner.activity.session, 1, owner.activity.bindingGeneration};
    bool isPublic = false;
    return catalog != nullptr
           && state::activity_sdk::resolve(catalog, selection, view)
                  == state::activity_sdk::Status::ready
           && state::activity_sdk::state_region_is_public(view, region, isPublic) && isPublic;
}

} // namespace

bool owner_scenario_wide(const Session& owner,
                         std::uint32_t objectTag,
                         std::uint32_t registryKey) noexcept {
    const state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    state::activity_sdk::BoundView view{};
    const state::activity_sdk::Selection selection{
        owner.activity.session, 1, owner.activity.bindingGeneration};
    bool wide = false;
    return catalog != nullptr
           && state::activity_sdk::resolve(catalog, selection, view)
                  == state::activity_sdk::Status::ready
           && state::activity_sdk::mission_seed_group_is_scenario_wide(
               view, objectTag, registryKey, wide)
           && wide;
}

bool publishes_on_public(const Session& owner,
                         const activity::host::ScriptableTarget& target) noexcept {
    // Publicity belongs to the state, not the bubble: an alternate state of a public bubble, such
    // as a cutscene, is private, and the client opens no public link while it stands there.
    const std::uint32_t bubble = target_bubble(target);
    return bubble < state::build_data::scenarios::kBubbleCapacity
           && owner_state_public(owner, target.stateLocalRegion)
           && !owner_scenario_wide(owner, target.objectTag, target.registryKey);
}

const Session* public_publisher_locked(const Session& owner,
                                       const activity::host::ScriptableTarget& target) noexcept {
    const std::uint32_t bubble = target_bubble(target);
    if (bubble >= state::build_data::scenarios::kBubbleCapacity) {
        return nullptr;
    }
    for (const Session& link : sessions()) {
        if (mission_seed_public_link(link, owner)
            && ((owner_public_bubbles(link, owner) >> bubble) & 1U) != 0
            && public_mission_owner_locked(link) == &owner) {
            return &link;
        }
    }
    return nullptr;
}

bool owner_publication_due_locked(const Session& link) noexcept {
    const Session* const owner = public_mission_owner_locked(link);
    if (owner == nullptr) {
        return false;
    }
    const std::uint64_t ownerBubbles = owner_public_bubbles(link, *owner);
    if (ownerBubbles == 0) {
        return false;
    }
    const std::uint64_t ownerGeneration = owner->activity.bindingGeneration;
    const PublicOwnerPublication& publication = link.activityPublicOwner;
    activity::host::AuthState ownerState{};
    if (activity::host::auth_state(owner->activity.session, ownerGeneration, ownerState)
        && (!state::activity::same_binding(publication.binding, owner->activity.session)
            || ownerState.estateRevision != publication.estateRevision)) {
        return true;
    }
    const MissionSeedLease& selected = owner->activityMissionSeed;
    const std::uint32_t selectedBubble =
        selected.plan.effectiveRegion / middleware::content::packages::tables::kSliceSetIndexFactor;
    const MissionSeedLease& lease = publication.seed;
    const bool selectionOwed =
        selected.configured && selected.scriptSelected
        && selected.bindingGeneration == ownerGeneration
        && selectedBubble < state::build_data::scenarios::kBubbleCapacity
        && ((ownerBubbles >> selectedBubble) & 1U) != 0
        && (!lease.configured || lease.bindingGeneration != link.activity.bindingGeneration
            || !same_mission_seed_plan(lease.plan, selected.plan)
            || (mission_seed_publication_pending(lease, link.activity.bindingGeneration)
                && lease.revision != lease.refusedRevision));
    if (selectionOwed) {
        return true;
    }
    activity::host::PendingScriptableOverride pending{};
    if (!activity::host::pending_scriptable_override_for_activity_client(
            owner->activity.session, ownerGeneration, pending)
        || !publishes_on_public(*owner, pending.target)) {
        return false;
    }
    const std::uint32_t pendingBubble =
        static_cast<std::uint32_t>(pending.target.stateLocalRegion)
        / middleware::content::packages::tables::kSliceSetIndexFactor;
    return ((ownerBubbles >> pendingBubble) & 1U) != 0;
}

bool mission_seed_region_published(const Session& link, std::uint32_t effectiveRegion) noexcept {
    // A public link carries the selection as the owner's publication, under its own generation.
    const auto& lease = link.activity.role == ActivityClientRole::publicTarget
                            ? link.activityPublicOwner.seed
                            : link.activityMissionSeed;
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
    // The plan numbers regions in the mission's world, as the public link's advertised region is.
    const std::uint64_t hosted = session.activity.role == ActivityClientRole::publicTarget
                                     ? advertised_bubble(session)
                                     : encrypted::push::activity::hosted_bubble_mask(session);
    return bubble < state::build_data::scenarios::kBubbleCapacity && ((hosted >> bubble) & 1U) != 0;
}

namespace {

/**
 * Reads whether a client stands in a private state of a bubble its private link does not host,
 * with that link's own receipt of the plan. The client opens no public link in a state its world
 * authors private, so no other receipt of it can arrive.
 */
[[nodiscard]] bool mission_seed_alternate_state_held(const Session& recipient,
                                                     const ActivityMissionSeedPlan& plan) noexcept {
    const auto& lease = recipient.activityMissionSeed;
    return recipient.activity.role == ActivityClientRole::privateCurrent
           && !owner_state_public(recipient, static_cast<std::int32_t>(plan.effectiveRegion))
           && lease.configured && lease.revision != 0
           && lease.bindingGeneration == recipient.activity.bindingGeneration
           && lease.revision == lease.publishedRevision && !lease.regionArrivalPending
           && same_mission_seed_plan(lease.plan, plan)
           && encrypted::push::activity::mission_seed_arrival_window_closed(
               state::activity::membership::player_region(recipient.activity.source.sessionId),
               plan.effectiveRegion);
}

} // namespace

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
        // hosts it and publishes this plan for it, so that link's receipt is the client's
        // acknowledgement of the bubble's content.
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
            if (!everyClient) {
                continue;
            }
            if (!mission_seed_alternate_state_held(recipient, plan)) {
                return false;
            }
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
    output.publicationRefused =
        output.publicationPending && lease.refusedRevision == lease.revision;
    output.regionArrivalPending = lease.configured && lease.regionArrivalPending;
}

} // namespace sunrise::server::bap
