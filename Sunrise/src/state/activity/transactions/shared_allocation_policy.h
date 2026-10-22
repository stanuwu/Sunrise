#pragma once

#include "../../account/public_profiles.h"
#include "../launch_party.h"
#include "../member_selection.h"

namespace sunrise::state::activity::transactions {
/** Only each enrolled owner's selected character can corroborate a native launch row. */
[[nodiscard]] inline bool capture_launch_party(const LaunchParty& party,
                                               LaunchPartyGuard& guard) noexcept {
    guard = {};
    if (!party.publisherAccount || !party.publisherCharacter
        || party.count > party.identities.size()) {
        return false;
    }
    AccountHandle publisher{};
    if (!account::profiles::find(party.publisherAccount, publisher)
        || account::profiles::selected_character(publisher) != party.publisherCharacter) {
        return false;
    }
    guard.publisherGeneration = account::profiles::membership_generation(publisher);
    std::size_t publisherCount{};
    for (std::size_t i = 0; i < party.count; ++i) {
        const auto& identity = party.identities[i];
        if (!identity.memberKey || !identity.accountSoid || !identity.opaqueSoid) {
            return false;
        }
        AccountHandle owner{};
        if (!account::profiles::find(identity.accountSoid, owner)
            || account::profiles::selected_character(owner) != identity.opaqueSoid) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (party.identities[j].memberKey == identity.memberKey
                || party.identities[j].accountSoid == identity.accountSoid) {
                return false;
            }
        }
        if (identity.accountSoid == party.publisherAccount) {
            if (identity.opaqueSoid != party.publisherCharacter) {
                return false;
            }
            ++publisherCount;
        }
        guard.generations[i] = account::profiles::membership_generation(owner);
    }
    return !party.count || publisherCount == 1;
}

/** Per-client selection nonces and descriptor echoes do not define a different authored world. */
[[nodiscard]] inline bool
same_launch_destination(const destination::DestinationSelection& left,
                        const destination::DestinationSelection& right) noexcept {
    return left.packageName == right.packageName
           && left.packageNameLength == right.packageNameLength
           && left.activityIndex == right.activityIndex
           && left.hasElementIndex == right.hasElementIndex
           && (!left.hasElementIndex || left.elementIndex == right.elementIndex)
           && left.hasArrivalBubbleOverride == right.hasArrivalBubbleOverride
           && (!left.hasArrivalBubbleOverride
               || left.arrivalBubbleOverride == right.arrivalBubbleOverride)
           && left.hasSliceSetOverride == right.hasSliceSetOverride
           && (!left.hasSliceSetOverride || left.sliceSetOverride == right.sliceSetOverride)
           && left.hasSpawnSetOverride == right.hasSpawnSetOverride
           && (!left.hasSpawnSetOverride || left.spawnSetOverride == right.spawnSetOverride);
}

/** Existing allocations keep their owner-character pair; another character starts a new launch. */
[[nodiscard]] inline std::size_t launch_owner_slot(const SessionRecord& record,
                                                   const LaunchParty& party) noexcept {
    std::size_t empty = record.launchOwners.size();
    for (std::size_t i = 0; i < record.launchOwners.size(); ++i) {
        const auto& owner = record.launchOwners[i];
        if (owner.accountSoid == party.publisherAccount) {
            return owner.characterSoid == party.publisherCharacter ? i : record.launchOwners.size();
        }
        if (!owner.accountSoid && empty == record.launchOwners.size()) {
            empty = i;
        }
    }
    return empty;
}

/** A different owner can share a live allocation only through a committed fireteam relation. */
[[nodiscard]] inline bool share_launch(const ActivityState& state,
                                       const SessionRecord& record,
                                       const LaunchParty& party) noexcept {
    if (!record.occupied || !record.bindingRetainCount || !record.launchParty.publisherAccount
        || (!record.joined && record.joinedRevision)
        || launch_owner_slot(record, party) == record.launchOwners.size()) {
        return false;
    }
    for (const auto& owner : record.launchOwners) {
        AccountHandle handle{};
        if (!owner.accountSoid || !account::profiles::find(owner.accountSoid, handle)
            || account::profiles::selected_character(handle) != owner.characterSoid) {
            continue;
        }
        if (owner.accountSoid == party.publisherAccount
            && owner.characterSoid == party.publisherCharacter) {
            return true;
        }
        if (state.fireteams.connected(owner.accountSoid, party.publisherAccount)) {
            return true;
        }
    }
    return false;
}
} // namespace sunrise::state::activity::transactions
