// Full retained Host Auth projected into one recipient's native roster.
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace layouts = state::build_data::scenarios;

const char* append_retained_auth_estate(
    const layouts::Definition& layout,
    const EffectiveRegion& region,
    Scratch& scratch,
    message::Snapshot& snapshot,
    std::span<const server::activity::host::PendingScriptableOverride> estate) noexcept {
    // Each included Auth slot is replaced, so snapshots must carry its complete retained value.
    // A connection's squad lease records its own deliveries, not the population's lifetime.
    // New observers and rejoined links need the shared spawn body even before combat Auth
    // exists. Reuse its exact generation; per-key group leases preserve registration revisions.
    for (const server::activity::host::PendingScriptableOverride& retained : estate) {
        if (retained.kind == server::activity::host::ScriptableOverrideKind::lifetime) {
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
                return "canonical_group_unknown";
            }
        }
        if (target.stateLocalRoster) {
            if (!layouts::valid_roster_group(retained.stateLocalRosterGroup)
                || target.stateLocalRegion < 0
                || target.rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
                || target.sdkObjectIndex == server::activity::host::kNoSdkObjectIndex) {
                return "retained_state_local_target";
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
                return "retained_group_install";
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
            return "retained_auth_apply";
        }
    }
    return nullptr;
}
} // namespace sunrise::server::bap::encrypted::push::activity
