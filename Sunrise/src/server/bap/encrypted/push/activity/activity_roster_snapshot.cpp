#include <Windows.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "../../../../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../../state/activity/destination/activity_destination_spawn_binding.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../../gameplay/group/group_host_sessions.h"
#include "activity_arrival.h"
#include "activity_mission_seed_roster.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace layouts = state::build_data::scenarios;
namespace squad = middleware::bap::activity_message::squad_auth;

/** Resolves the one region a session publishes. */
EffectiveRegion effective_region(const state::activity::SessionBinding& binding) noexcept {
    EffectiveRegion region{};
    region.index = state::activity::membership::kAbsentRegionIndex;
    if (!state::activity::binding_matches(binding)) {
        return region;
    }
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    const state::activity::destination::DestinationSelection& selection = binding.destination;
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    // A missing layout leaves a cleared definition, and the arrival rule then returns the
    // authored fallback index.
    layouts::Definition layout{};
    static_cast<void>(state::build_data::find_scenario_layout(name, layout));
    region.arrival = arrival_slice_set(defaults.defaultDestination, selection, name, layout);
    const std::int32_t reported = state::activity::membership::player_region(binding.sessionId);
    region.reported = reported >= 0;
    region.index = region.reported ? reported : static_cast<std::int32_t>(region.arrival);
    return region;
}

/** Resolves the exact region one selected BAP ActivityClient would put in msg 5. */
EffectiveRegion selected_effective_region(const Session& session, std::uint16_t arrival) noexcept {
    if (session.activity.role == ActivityClientRole::none
        || !state::activity::binding_matches(session.activity.session)
        || !state::activity::binding_matches(session.activity.source)) {
        EffectiveRegion region{};
        region.index = state::activity::membership::kAbsentRegionIndex;
        region.arrival = arrival;
        return region;
    }
    // The region the client is in. Its pending leg only names where it is heading, and after a
    // z-leg switch it names the region behind the player.
    const std::int32_t privateReportedRegion =
        session.activity.role == ActivityClientRole::privateCurrent
            ? state::activity::membership::player_region(session.activity.source.sessionId)
            : state::activity::membership::kAbsentRegionIndex;
    return select_activity_client_region(
        session.activity.role, privateReportedRegion, session.activity.advertisedRegion, arrival);
}

/** Reads where the client says it is. */
state::activity::membership::ClientPlacement
client_placement(const Session& session, const RefreshReport* refresh) noexcept {
    state::activity::membership::ClientPlacement placement =
        state::activity::membership::reported_placement(session.activity.session.sessionId);
    // Staging runs before the commit, so the refresh being answered is not in State yet.
    if (refresh != nullptr) {
        placement.bubble = refresh->bubble;
        placement.bubbleRevision = refresh->revision;
        if (refresh->hasCurrentRegion) {
            placement.currentRegion = refresh->currentRegion;
        }
    }
    return placement;
}

/** Tests whether the client holds a slice set and no host move is due. */
bool client_region_ready(const Session& session, const RefreshReport* refresh) noexcept {
    const state::activity::membership::ClientPlacement placement =
        client_placement(session, refresh);
    const std::int32_t held = state::activity::membership::instantiated_region(placement);
    const MissionSeedLease& lease = session.activityMissionSeed;
    // A move is pending only while the client is somewhere other than the region the selection
    // names. A selection naming the region it already holds moves nobody, and arming the gate
    // behind a player who has arrived flashed their loading screen for one publish tick.
    const bool movePending = lease.configured
                             && lease.bindingGeneration == session.activity.bindingGeneration
                             && lease.regionArrivalPending
                             && static_cast<std::int64_t>(lease.plan.effectiveRegion) != held;
    return !movePending && held >= 0;
}

/** Tests whether the client has reported arrival in its instantiated region. */
bool client_in_world(const Session& session, const RefreshReport* refresh) noexcept {
    // ws-702 world state 8 is the arrival report. It does not depend on the player spawn.
    const state::activity::membership::ClientPlacement placement =
        client_placement(session, refresh);
    return placement.entered && client_region_ready(session, refresh);
}

/** Merges one staged squad body after the complete cumulative frame reached transport output. */
bool activate_staged_squad_override(Session& session) noexcept {
    const RosterPublication& staged = session.activityRosterStaged;
    const server::activity::host::PendingScriptableOverride& pending = staged.scriptableOverride;
    const server::activity::host::ScriptableTarget& target = pending.target;
    if (!staged.staged || !staged.hasScriptableOverride || !staged.activatesSquadOverride
        || staged.bindingGeneration == 0
        || staged.bindingGeneration != session.activity.bindingGeneration
        || pending.expectedActivityClientGeneration != staged.bindingGeneration
        || pending.kind != server::activity::host::ScriptableOverrideKind::squad
        || (target.stateLocalRoster && !staged.hasSquadStateSequence)) {
        return false;
    }
    RetainedSquadAuth retained{};
    if (!make_retained_squad_auth(pending, session.activity.bindingGeneration, retained)) {
        return false;
    }
    if (target.stateLocalRoster) {
        const layouts::RosterGroup& group = pending.stateLocalRosterGroup;
        if (staged.stateLocalRegion != target.stateLocalRegion
            || !layouts::valid_roster_group(group)
            || target.rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
            || target.sdkObjectIndex == server::activity::host::kNoSdkObjectIndex
            || group.objectTag != target.objectTag || group.registryKey != target.registryKey
            || target.rosterSlotOffset >= group.slotCount
            || group.slotTypes[target.rosterSlotOffset] != target.slotType
            || group.slotIndices[target.rosterSlotOffset] != target.slotIndex
            || (group.slotFlags[target.rosterSlotOffset] & message::kSlotAuthFlag) == 0) {
            return false;
        }
    } else if (target.stateLocalRegion >= 0) {
        return false;
    }

    SquadOverrideLease& lease = session.activitySquadOverride;
    if (!lease.active) {
        SecureZeroMemory(&lease, sizeof lease);
        lease.groups[0].scopeTarget = target;
        if (target.stateLocalRoster) {
            lease.groups[0].stateLocalRosterGroup = pending.stateLocalRosterGroup;
        }
        lease.groups[0].region = target.stateLocalRegion;
        lease.groups[0].authCount = 1;
        lease.groups[0].stateSequence = staged.squadStateSequence;
        lease.groups[0].regionEpoch = session.activityRosterRegionEpoch;
        retained.groupIndex = 0;
        lease.authBodies[0] = retained;
        lease.bindingGeneration = session.activity.bindingGeneration;
        lease.authCount = 1;
        lease.groupCount = 1;
        lease.active = true;
        return true;
    }
    if (!valid_retained_squad_lease(lease, session.activity.bindingGeneration)) {
        return false;
    }
    const std::size_t groupIndex = retained_group_index(lease, target);
    if (groupIndex < lease.groupCount) {
        RetainedSquadGroup& group = lease.groups[groupIndex];
        if (target.stateLocalRoster
            && (!same_generated_group(group.stateLocalRosterGroup, pending.stateLocalRosterGroup)
                || staged.squadStateSequence != group.stateSequence)) {
            return false;
        }
        retained.groupIndex = static_cast<std::uint8_t>(groupIndex);
        for (std::size_t index = 0; index < lease.authCount; ++index) {
            if (lease.authBodies[index].groupIndex != groupIndex
                || !same_retained_target(group, lease.authBodies[index], target)) {
                continue;
            }
            lease.authBodies[index] = retained;
            return true;
        }
        if (lease.authCount >= lease.authBodies.size()
            || (target.stateLocalRoster
                && group.authCount >= group.stateLocalRosterGroup.slotCount)) {
            return false;
        }
        lease.authBodies[lease.authCount] = retained;
        ++lease.authCount;
        ++group.authCount;
        return true;
    }
    if (lease.groupCount >= lease.groups.size() || lease.authCount >= lease.authBodies.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        if (lease.groups[index].scopeTarget.registryKey == target.registryKey) {
            return false;
        }
    }
    RetainedSquadGroup& group = lease.groups[lease.groupCount];
    group = {};
    group.scopeTarget = target;
    if (target.stateLocalRoster) {
        group.stateLocalRosterGroup = pending.stateLocalRosterGroup;
    }
    group.region = target.stateLocalRegion;
    group.authCount = 1;
    group.stateSequence = staged.squadStateSequence;
    group.regionEpoch = session.activityRosterRegionEpoch;
    retained.groupIndex = static_cast<std::uint8_t>(lease.groupCount);
    lease.authBodies[lease.authCount] = retained;
    ++lease.authCount;
    ++lease.groupCount;
    return true;
}

/** Restores counters from one discarded publication while leaving its committed lease intact. */
void rollback_staged_roster_state(Session& session) noexcept {
    const RosterPublication& staged = session.activityRosterStaged;
    if (!staged.staged) {
        return;
    }
    session.activityRosterGroupLeases = staged.priorLeases;
    session.activityRosterSends = staged.priorSends;
    session.activityRosterState = staged.priorState;
    session.activityRosterRegionEpoch = staged.priorRegionEpoch;
    session.activityRosterRegionBubble = staged.priorRegionBubble;
    session.activityRosterOwedForEpoch = staged.priorRosterOwedForEpoch;
    session.activityRosterStaged = {};
}

/** Resolves the region one prepared membership body publishes. */
EffectiveRegion planned_region(const state::activity::membership::PendingMutation& mutation,
                               const state::activity::SessionBinding& binding) noexcept {
    EffectiveRegion region = effective_region(binding);
    // A pending leg naming a region is where the client is heading, so the body advertises it.
    // That holds whether the leg arrived in this delta or in an earlier report. A negative one
    // is a completed transition, and the committed position stands.
    const std::int32_t pending =
        mutation.authoritativeInput.hasRegion
            ? mutation.authoritativeInput.region.index
            : state::activity::membership::reported_region(binding.sessionId);
    if (pending > state::activity::membership::kAbsentRegionIndex) {
        region.index = pending;
        region.reported = true;
    }
    return region;
}

/** Resolves the region one private link's membership body publishes. */
EffectiveRegion private_planned_region(const state::activity::membership::PendingMutation& mutation,
                                       const state::activity::SessionBinding& binding) noexcept {
    EffectiveRegion region = planned_region(mutation, binding);
    if (region.reported) {
        return region;
    }
    server::gameplay::group::HostSessionBinding host{};
    if (server::gameplay::private_host_session(binding, host) && host.regionIndex >= 0) {
        region.index = host.regionIndex;
        region.reported = true;
    }
    return region;
}

/** Lists the regions one membership body advertises a host for. */
void directory_regions(const state::activity::SessionBinding& binding,
                       std::int32_t regionIndex,
                       std::span<std::int32_t> output,
                       std::size_t& count) noexcept {
    namespace tables = middleware::content::packages::tables;
    count = 0;
    if (output.empty() || regionIndex <= state::activity::membership::kAbsentRegionIndex
        || !state::activity::binding_matches(binding)) {
        return;
    }
    output[count] = regionIndex;
    ++count;
    const state::activity::destination::DestinationSelection& selection = binding.destination;
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return;
    }
    const std::size_t bubbles =
        (std::min)(static_cast<std::size_t>(layout.bubbleCount), layout.bubbleStates.size());
    for (std::size_t bubble = 0; bubble < bubbles && count < output.size(); ++bubble) {
        // A bubble with no slice-set state has no slice set, so nothing can be hosted there.
        if (layout.bubbleStates[bubble] != layouts::kBubbleEnabledByte) {
            continue;
        }
        const auto region =
            static_cast<std::int32_t>(tables::region_index(static_cast<std::uint32_t>(bubble)));
        // One record per bubble, so the published region already speaks for its own bubble. Adding
        // that bubble's state-zero region too would ask for two records in one slot.
        if (static_cast<std::size_t>(regionIndex) / tables::kSliceSetIndexFactor == bubble) {
            continue;
        }
        output[count] = region;
        ++count;
    }
}

/** Builds the roster body input for one session's current destination. */
RosterOutcome
build_roster_snapshot(Session& session,
                      Scratch& scratch,
                      message::Snapshot& snapshot,
                      std::span<char> destination,
                      std::size_t& destinationLength,
                      const middleware::bap::activity_message::patch_epoch::PatchEpoch* epoch,
                      std::uint8_t lifetimeState,
                      const message::AuthOverride* authOverride,
                      std::uint16_t rosterGroupIndex,
                      std::uint16_t rosterSlotOffset,
                      bool stateLocalRosterTarget,
                      std::int32_t stateLocalRegion,
                      std::uint32_t sdkObjectIndex,
                      const layouts::RosterGroup* stateLocalRosterGroup,
                      const EffectiveRegion* exactRegion,
                      const RefreshReport* refresh,
                      std::span<const TailAuthOverride> tailOverrides) noexcept {
    snapshot = {};
    destinationLength = 0;
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    if (session.activity.role == ActivityClientRole::none
        || !state::activity::binding_matches(session.activity.session)
        || !state::activity::binding_matches(session.activity.source)) {
        return RosterOutcome::noLayout;
    }
    // Public targets keep the destination copied from their exact advertised source generation.
    const state::activity::destination::DestinationSelection& selection =
        session.activity.session.destination;
    layouts::Definition layout{};
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    destinationLength = (std::min)(name.size(), destination.size());
    std::copy_n(name.begin(), destinationLength, destination.begin());
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return RosterOutcome::noLayout;
    }
    const std::uint64_t hostedBubbles = hosted_bubble_mask(session);
    if (!fill_roster(layout, hostedBubbles, scratch, snapshot.roster)) {
        return RosterOutcome::noGroups;
    }

    const state::activity::defaults::FallbackPolicy& fallback =
        defaults.defaultDestination.fallback;
    // One resolution serves this body and the citizen advertisement in message 12. Two would let
    // the join descriptor land in a region record the client is not pending on.
    const EffectiveRegion committedRegion = selected_effective_region(
        session, arrival_slice_set(defaults.defaultDestination, selection, name, layout));
    const EffectiveRegion region = exactRegion == nullptr ? committedRegion : *exactRegion;
    if (exactRegion != nullptr
        && (session.activity.role != ActivityClientRole::privateCurrent || !region.reported
            || region.arrival != committedRegion.arrival)) {
        return refuse_override("exact_region");
    }
    if (region.index < 0) {
        return RosterOutcome::noLayout;
    }
    std::vector<server::activity::host::PendingScriptableOverride> authEstate{};
    if (!server::activity::host::scriptable_auth_estate(
            session.activity.session, session.activity.bindingGeneration, authEstate)) {
        return refuse_override("auth_estate");
    }
    const std::size_t canonicalGroupCount = snapshot.roster.groupCount;
    if (append_initial_mission_seed(session,
                                    scratch,
                                    snapshot,
                                    static_cast<std::uint32_t>(region.index),
                                    hostedBubbles,
                                    canonicalGroupCount,
                                    refresh)
        == MissionSeedRosterResult::refused) {
        return refuse_override("mission_seed");
    }
    // Retained and pending groups below may only match a group the seed published.
    const std::size_t seedGroupCount = snapshot.roster.groupCount;
    const bool pendingStateLocal = authOverride != nullptr && stateLocalRosterTarget;
    if (session.activitySquadOverride.active
        && session.activitySquadOverride.bindingGeneration != session.activity.bindingGeneration) {
        SecureZeroMemory(&session.activitySquadOverride, sizeof session.activitySquadOverride);
    }
    const bool retainedSquad = session.activitySquadOverride.active;
    const SquadOverrideLease& lease = session.activitySquadOverride;
    if (retainedSquad && !valid_retained_squad_lease(lease, session.activity.bindingGeneration)) {
        return refuse_override("retained_lease");
    }
    server::activity::host::ScriptableTarget pendingTarget{};
    if (authOverride != nullptr) {
        pendingTarget.objectTag = authOverride->objectTag;
        pendingTarget.registryKey = authOverride->key;
        pendingTarget.authSchema = authOverride->authSchema;
        pendingTarget.rosterGroupIndex = rosterGroupIndex;
        pendingTarget.rosterSlotOffset = rosterSlotOffset;
        pendingTarget.slotIndex = authOverride->slotIndex;
        pendingTarget.sdkObjectIndex = sdkObjectIndex;
        pendingTarget.stateLocalRegion = stateLocalRegion;
        pendingTarget.slotType = authOverride->slotType;
        pendingTarget.stateLocalRoster = stateLocalRosterTarget;
    }
    const bool squadOverride = authOverride != nullptr && authOverride->slotType == squad::kSlotType
                               && authOverride->authSchema == squad::kSchema;
    const std::size_t pendingGroupIndex = retainedSquad && authOverride != nullptr
                                              ? retained_group_index(lease, pendingTarget)
                                              : lease.groupCount;
    if (retainedSquad && authOverride != nullptr) {
        if (pendingGroupIndex < lease.groupCount) {
            const RetainedSquadGroup& retainedGroup = lease.groups[pendingGroupIndex];
            if (stateLocalRosterTarget
                && (stateLocalRosterGroup == nullptr
                    || !same_generated_group(*stateLocalRosterGroup,
                                             retainedGroup.stateLocalRosterGroup))) {
                return refuse_override("retained_group_mismatch");
            }
        } else if (squadOverride
                   && (lease.groupCount >= lease.groups.size()
                       || lease.authCount >= lease.authBodies.size())) {
            return refuse_override("lease_capacity");
        }
    }
    if (pendingStateLocal
        && (stateLocalRosterGroup == nullptr
            || rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
            || sdkObjectIndex == server::activity::host::kNoSdkObjectIndex)) {
        return refuse_override("pending_state_local_target");
    }
    std::array<std::size_t, message::kPublishedGroupCapacity> retainedGroupPositions{};
    retainedGroupPositions.fill(snapshot.roster.groups.size());
    for (std::size_t index = 0; retainedSquad && index < lease.groupCount; ++index) {
        const RetainedSquadGroup& retainedGroup = lease.groups[index];
        if (!retainedGroup.scopeTarget.stateLocalRoster) {
            continue;
        }
        if (!msg1_selects_region(layout, retainedGroup.region)) {
            return refuse_override("retained_region");
        }
        const std::uint32_t bubble = static_cast<std::uint32_t>(retainedGroup.region)
                                     / middleware::content::packages::tables::kSliceSetIndexFactor;
        std::size_t position = snapshot.roster.groups.size();
        const ExistingGroup existing = find_existing_group(
            retainedGroup.stateLocalRosterGroup, scratch, snapshot.roster, position);
        if (existing == ExistingGroup::conflict
            || (existing == ExistingGroup::exact && position >= seedGroupCount)
            || (existing == ExistingGroup::exact
                && !activate_existing_group(position, bubble, scratch, snapshot.roster))) {
            return refuse_override("retained_existing_group");
        }
        if (existing == ExistingGroup::missing) {
            position = snapshot.roster.groupCount;
            if (!append_state_local_group(
                    retainedGroup.stateLocalRosterGroup, bubble, scratch, snapshot.roster)) {
                return refuse_override("retained_append");
            }
        }
        retainedGroupPositions[index] = position;
    }
    const bool pendingAddsGroup = pendingStateLocal && pendingGroupIndex == lease.groupCount;
    std::size_t pendingGroupPosition = snapshot.roster.groups.size();
    if (!pendingAddsGroup && pendingStateLocal && pendingGroupIndex < lease.groupCount) {
        pendingGroupPosition = retainedGroupPositions[pendingGroupIndex];
    }
    for (std::size_t index = 0; retainedSquad && index < lease.authCount; ++index) {
        message::AuthOverride retainedAuth{};
        if (!retained_squad_auth(lease, index, retainedAuth)) {
            return refuse_override("retained_auth");
        }
        const RetainedSquadAuth& retained = lease.authBodies[index];
        const RetainedSquadGroup& retainedGroup = lease.groups[retained.groupIndex];
        const bool replace =
            authOverride != nullptr && same_retained_target(retainedGroup, retained, pendingTarget);
        const message::AuthOverride& effectiveAuth = replace ? *authOverride : retainedAuth;
        const std::uint16_t effectiveGroup =
            replace ? rosterGroupIndex : retainedGroup.scopeTarget.rosterGroupIndex;
        const std::uint16_t effectiveSlot = replace ? rosterSlotOffset : retained.rosterSlotOffset;
        const bool effectiveStateLocal =
            replace ? stateLocalRosterTarget : retainedGroup.scopeTarget.stateLocalRoster;
        if (!install_auth_override(layout,
                                   region,
                                   scratch,
                                   snapshot,
                                   effectiveAuth,
                                   effectiveGroup,
                                   effectiveSlot,
                                   effectiveStateLocal)) {
            return refuse_override("retained_auth_install");
        }
    }
    // Message 5 resets every registered Auth slot before applying its bodies, so the complete
    // latest-per-ClientRef estate is re-emitted and a later action cannot erase an earlier one.
    // Squads stay in their lease, which also owns group admission and per-group revisions.
    for (const server::activity::host::PendingScriptableOverride& retained : authEstate) {
        if (retained.kind == server::activity::host::ScriptableOverrideKind::squad
            || retained.kind == server::activity::host::ScriptableOverrideKind::lifetime) {
            continue;
        }
        // A dialogue body is a pulse: the client plays one line of the cue each time it applies
        // the body, and it applies every body of every msg 5. So it goes out once, in the push
        // that delivers it, and the slot then carries no body.
        if (retained.kind == server::activity::host::ScriptableOverrideKind::dialogue) {
            continue;
        }
        const server::activity::host::ScriptableTarget& target = retained.target;
        if (!target.stateLocalRoster) {
            const CanonicalGroupStatus status =
                canonical_group_status(layout, region, target.rosterGroupIndex);
            if (status == CanonicalGroupStatus::inactive) {
                continue;
            }
            if (status == CanonicalGroupStatus::unknown) {
                return refuse_override("canonical_group_unknown");
            }
        }
        if (target.stateLocalRoster) {
            if (!layouts::valid_roster_group(retained.stateLocalRosterGroup)
                || target.stateLocalRegion < 0
                || target.rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
                || target.sdkObjectIndex == server::activity::host::kNoSdkObjectIndex) {
                return refuse_override("retained_state_local_target");
            }
            const std::uint32_t bubble =
                static_cast<std::uint32_t>(target.stateLocalRegion)
                / middleware::content::packages::tables::kSliceSetIndexFactor;
            std::size_t position = snapshot.roster.groups.size();
            const ExistingGroup existing = find_existing_group(
                retained.stateLocalRosterGroup, scratch, snapshot.roster, position);
            if (existing == ExistingGroup::conflict
                || (existing == ExistingGroup::exact
                    && !activate_existing_group(position, bubble, scratch, snapshot.roster))
                || (existing == ExistingGroup::missing
                    && !append_state_local_group(
                        retained.stateLocalRosterGroup, bubble, scratch, snapshot.roster))) {
                return refuse_override("retained_group_install");
            }
        }
        message::AuthOverride value{};
        if (!make_auth_override(retained, value)
            || !install_auth_override(layout,
                                      region,
                                      scratch,
                                      snapshot,
                                      value,
                                      target.rosterGroupIndex,
                                      target.rosterSlotOffset,
                                      target.stateLocalRoster)) {
            return refuse_override("retained_auth_apply");
        }
    }
    // The pending override's group goes after the retained estate, where the next push will place
    // it once retained, so two pushes carry the same group order. An exact match here may be a
    // group the estate just appended, so no position bound applies.
    if (pendingAddsGroup) {
        const std::uint32_t bubble = static_cast<std::uint32_t>(stateLocalRegion)
                                     / middleware::content::packages::tables::kSliceSetIndexFactor;
        ExistingGroup existing = ExistingGroup::conflict;
        if (stateLocalRosterGroup != nullptr) {
            existing = find_existing_group(
                *stateLocalRosterGroup, scratch, snapshot.roster, pendingGroupPosition);
        }
        if (existing == ExistingGroup::conflict
            || (existing == ExistingGroup::exact
                && !activate_existing_group(
                    pendingGroupPosition, bubble, scratch, snapshot.roster))) {
            return refuse_override("pending_existing_group");
        }
        if (existing == ExistingGroup::missing) {
            pendingGroupPosition = snapshot.roster.groupCount;
            if (!append_state_local_group(
                    *stateLocalRosterGroup, bubble, scratch, snapshot.roster)) {
                return refuse_override("pending_append");
            }
        }
    }
    // The pending body applies last, so it wins over an older estate body for the same target.
    if (authOverride != nullptr
        && !install_auth_override(layout,
                                  region,
                                  scratch,
                                  snapshot,
                                  *authOverride,
                                  rosterGroupIndex,
                                  rosterSlotOffset,
                                  stateLocalRosterTarget)) {
        return refuse_override("pending_auth_apply");
    }
    // A burst commits behind the head and leaves on this body with it.
    for (const TailAuthOverride& queued : tailOverrides) {
        if (!install_auth_override(layout,
                                   region,
                                   scratch,
                                   snapshot,
                                   queued.value,
                                   queued.rosterGroupIndex,
                                   queued.rosterSlotOffset,
                                   queued.stateLocalRosterTarget)) {
            return refuse_override("tail_auth_apply");
        }
    }
    // Read from the merged estate, so a pending disable wins over a retained enable.
    namespace darkness = middleware::bap::activity_message::darkness_zone;
    for (const auto& value : snapshot.authOverrides) {
        bool enabled = false;
        if (value.sdkCompiled && value.present && value.slotType == darkness::kSlotType
            && value.authSchema == darkness::kSchema && value.byteCount <= value.body.size()
            && darkness::read_enabled(
                std::span(value.body).first(value.byteCount), value.bitCount, enabled)) {
            snapshot.hasDarknessPolicy = true;
            snapshot.darknessEnabled = enabled;
        }
    }
    // Staging runs before the connection field is published, so a body answering message 52 has to
    // take the epoch from that message instead of from the connection.
    snapshot.patchEpoch = epoch != nullptr ? *epoch : session.activityPatchEpoch.value;
    // The character the join named wins, resolved to its authored SOID. The client binds its
    // player by matching this value against the object registry, and the short form the join
    // carries matches nothing.
    snapshot.playerKey = published_player_key(session);
    snapshot.lifetime = lifetimeState;
    // Hold the native spawn gate until the ws-702 world state reads 8. A spawn before the fade
    // arms leaves the screen black.
    snapshot.awaitClientSync = !client_in_world(session, refresh);
    // Player_BindComponents walks every type-13 reference and the player datum can name any one of
    // them. So every participation record carries the same player key. Selecting the first slot
    // leaves the authored cinematic participant unbound whenever it names another record.
    snapshot.keyOnEveryParticipationSlot = true;
    // The participation record's `+0` latches only when the region index is known.
    snapshot.region = static_cast<std::uint32_t>(region.index);
    snapshot.hasRegion = true;
    // The override must name the exact slice set the client is in, so it follows the published
    // region and is never floored to the bubble's first state. A pair naming the arrival is inert
    // after a teleport, and the picker then falls back to an arbitrary point.
    snapshot.spawnSliceSet =
        region.index >= 0 ? static_cast<std::uint32_t>(region.index) : region.arrival;
    snapshot.spawnSetHash =
        state::activity::destination::attachable_spawn_set_hash(selection, fallback.spawnSetHash);
    // An armed wipe respawns at its checkpoint spawn set, not at the arrival override.
    const std::uint32_t checkpoint = state::activity::membership::checkpoint_spawn_hash(
        session.activity.source.sessionId, region.index);
    if (checkpoint != 0) {
        snapshot.spawnSetHash = checkpoint;
    }
    snapshot.hasSpawnOverride =
        snapshot.spawnSetHash != 0 && snapshot.spawnSetHash != message::kAbsentSpawnSetHash;
    advance_region_epoch(session, refresh);
    stamp_group_sequences(session, snapshot.roster);
    snapshot.stateSequence = session.activityRosterState;
    // One-shot first-roster flag; the mission seed's adoption guard reads it.
    if (session.activityRosterSends == 0) {
        session.activityRosterSends = 1;
    }
    // A retained key holds its revision, so a later Auth change cannot re-register the group and
    // rebuild its actors. A bubble change re-arms it once per epoch; the client unloads the group
    // with the bubble.
    for (std::size_t index = 0; retainedSquad && index < lease.groupCount; ++index) {
        const std::size_t position = retainedGroupPositions[index];
        if (position >= snapshot.roster.groupCount) {
            continue;
        }
        RetainedSquadGroup& retained = session.activitySquadOverride.groups[index];
        if (retained.regionEpoch == session.activityRosterRegionEpoch) {
            snapshot.roster.groups[position].stateSequence = retained.stateSequence;
            continue;
        }
        retained.regionEpoch = session.activityRosterRegionEpoch;
        retained.stateSequence = snapshot.roster.groups[position].stateSequence;
    }
    if (pendingStateLocal && pendingGroupPosition >= snapshot.roster.groupCount) {
        return refuse_override("pending_group_position");
    }
    std::size_t senseCount = 0;
    for (const message::AuthOverride& auth : snapshot.authOverrides) {
        if (auth.slotType != 1) {
            continue;
        }
        server::activity::host::SenseObservationKey key{};
        key.registryKey = auth.key;
        key.objectTag = auth.objectTag;
        key.senseSchema = 0x80807ECCU;
        key.slotIndex = auth.slotIndex;
        key.slotType = auth.slotType;
        middleware::bap::activity_message::squad_sense::State recovered{};
        if (!server::activity::host::snapshot_squad_sense(
                session.activity.session, session.activity.bindingGeneration, key, recovered)) {
            continue;
        }
        message::SenseOverride& sense = scratch.rosterSenseOverrides[senseCount];
        sense = {};
        if (!middleware::bap::activity_message::squad_sense::encode(
                recovered, sense.body, sense.byteCount, sense.bitCount)) {
            return refuse_override("squad_sense");
        }
        sense.key = auth.key;
        sense.objectTag = auth.objectTag;
        sense.slotIndex = auth.slotIndex;
        sense.slotType = auth.slotType;
        sense.counter = recovered.counter;
        ++senseCount;
    }
    snapshot.senseOverrides = std::span(scratch.rosterSenseOverrides).first(senseCount);
    return RosterOutcome::published;
}

/** The player key this link's message 5 binds: the join character's SOID, or its identity. */
std::uint64_t published_player_key(const Session& session) noexcept {
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    std::uint64_t key = roster_player_key(session.activityCharacterSoid);
    if (defaults.rosterKeyFromIdentity) {
        const std::uint64_t identity =
            state::activity::membership::join_identity(session.activity.session.sessionId);
        if (identity != 0) {
            key = identity;
        }
    }
    return key;
}

} // namespace sunrise::server::bap::encrypted::push::activity
