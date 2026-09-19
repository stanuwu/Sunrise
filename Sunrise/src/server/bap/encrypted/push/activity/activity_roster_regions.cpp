// Region resolution for the msg-5 roster body and the membership directory.

#include <algorithm>
#include <span>
#include <string_view>

#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../../gameplay/group/group_host_sessions.h"
#include "activity_arrival.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace layouts = state::build_data::scenarios;

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
    region.arrival =
        arrival_slice_set(defaults.defaultDestination,
                          selection,
                          name,
                          layout,
                          state::activity::membership::declared_initial_region(binding.sessionId));
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
    // The row was claimed for the arrival known at the join. A program attaching later can move
    // the arrival, and the row cannot follow while a delivered body retains it, so it yields.
    if (server::gameplay::private_host_session(binding, host) && host.regionIndex >= 0
        && host.regionIndex == static_cast<std::int32_t>(region.arrival)) {
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

} // namespace sunrise::server::bap::encrypted::push::activity
