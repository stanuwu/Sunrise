// Full retained Host Auth projected into one recipient's native roster.
#include "../../../../../core/logging/log.h"
#include "../../../activity_mission_seed_lease.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace layouts = state::build_data::scenarios;

void report_owner_key_conflict(const Session* owner,
                               const server::activity::host::ScriptableTarget& target) noexcept {
    if (owner == nullptr) {
        return;
    }
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=activity stage=owner_key_conflict key=0x%08X object=0x%08X region=%d",
                      target.registryKey,
                      target.objectTag,
                      target.stateLocalRegion);
}

namespace {

/**
 * @return True when this pass carries the target: a public link carries only its owner's
 * state-local targets in the bubbles it publishes for it, and a private link carries every
 * target except those its public link publishes.
 */
[[nodiscard]] bool
estate_carries(const Session& link,
               const Session* owner,
               std::uint64_t ownerBubbles,
               const server::activity::host::PendingScriptableOverride& retained) noexcept {
    const server::activity::host::ScriptableTarget& target = retained.target;
    if (owner != nullptr) {
        const std::uint32_t bubble = static_cast<std::uint32_t>(target.stateLocalRegion)
                                     / middleware::content::packages::tables::kSliceSetIndexFactor;
        return target.stateLocalRoster && target.stateLocalRegion >= 0
               && bubble < layouts::kBubbleCapacity && ((ownerBubbles >> bubble) & 1U) != 0
               && publishes_on_public(*owner, target);
    }
    if (link.activity.role != ActivityClientRole::privateCurrent
        || !publishes_on_public(link, target)) {
        return true;
    }
    return false;
}

} // namespace

const char* append_retained_auth_estate(
    const Session& link,
    const Session* owner,
    std::uint64_t ownerBubbles,
    const layouts::Definition& layout,
    const EffectiveRegion& region,
    Scratch& scratch,
    message::Snapshot& snapshot,
    std::span<const server::activity::host::PendingScriptableOverride> estate,
    std::uint64_t deliveredDialogueRevision) noexcept {
    // Each included Auth slot is replaced, so snapshots must carry its complete retained value.
    // A connection's squad lease records its own deliveries, not the population's lifetime.
    // New observers and rejoined links need the shared spawn body even before combat Auth
    // exists. Reuse its exact generation; per-key group leases preserve registration revisions.
    for (const server::activity::host::PendingScriptableOverride& retained : estate) {
        if (retained.kind == server::activity::host::ScriptableOverrideKind::lifetime) {
            continue;
        }
        // A dialogue body is a pulse: the client plays one line of the cue each time it applies
        // the body, and it applies every body of every msg 5. So each link carries it once, in
        // its first body after the pulse was retained, and its later bodies leave the slot out.
        if (retained.kind == server::activity::host::ScriptableOverrideKind::dialogue
            && retained.revision <= deliveredDialogueRevision) {
            continue;
        }
        if (!estate_carries(link, owner, ownerBubbles, retained)) {
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
            if (existing == ExistingGroup::conflict) {
                report_owner_key_conflict(owner, target);
                return "retained_group_install";
            }
            if ((existing == ExistingGroup::exact
                 && !activate_existing_group(position, bubble, scratch, snapshot.roster))
                || (existing == ExistingGroup::missing
                    && !append_state_local_group(
                        retained.stateLocalRosterGroup, bubble, scratch, snapshot.roster))
                || (owner != nullptr && !note_owner_key(scratch, target.registryKey))) {
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
