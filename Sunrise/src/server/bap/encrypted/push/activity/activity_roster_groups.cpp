// Group assembly and lease checks for the msg-5 roster body.
// Every helper here runs under the connection lock, and Scratch owns the storage its spans name.

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity_sdk/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace layouts = state::build_data::scenarios;
namespace squad = middleware::bap::activity_message::squad_auth;

namespace {
/** Only a type-13 slot binds the player, so only a group holding one may carry the key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** The join request names its character in the low half of the SOID, so compare on that half. */
constexpr std::uint64_t kIdentityLowMask = 0xFFFFFFFFULL;

/**
 * Copies one roster group into the encoder's fixed input.
 * @param tableIndex Roster table index the destination row names.
 * @param scratch Lock-owned roster group storage the spans point into.
 * @param slot Storage and input slot to fill, which are the same ordinal.
 * @param roster Receives the group.
 * @return True when the named group was found.
 */
[[nodiscard]] bool fill_group(std::uint16_t tableIndex,
                              Scratch& scratch,
                              std::size_t slot,
                              message::Roster& roster) noexcept {
    layouts::RosterGroup& group = scratch.rosterGroups[slot];
    if (!state::build_data::find_roster_group(tableIndex, group)) {
        return false;
    }
    roster.groups[slot].objectTag = group.objectTag;
    roster.groups[slot].key = group.registryKey;
    roster.groups[slot].slotTypes =
        std::span<const std::uint8_t>(group.slotTypes.data(), group.slotCount);
    roster.groups[slot].slotFlags =
        std::span<const std::uint8_t>(group.slotFlags.data(), group.slotCount);
    roster.groups[slot].slotIndices =
        std::span<const std::uint16_t>(group.slotIndices.data(), group.slotCount);
    return true;
}

} // namespace

/** Copies one request-owned generated group into the encoder's fixed input. */
[[nodiscard]] bool fill_generated_group(const layouts::RosterGroup& source,
                                        Scratch& scratch,
                                        std::size_t slot,
                                        message::Roster& roster) noexcept {
    if (!layouts::valid_roster_group(source) || slot >= scratch.rosterGroups.size()
        || slot >= roster.groups.size()) {
        return false;
    }
    layouts::RosterGroup& group = scratch.rosterGroups[slot];
    group = source;
    roster.groups[slot].objectTag = group.objectTag;
    roster.groups[slot].key = group.registryKey;
    roster.groups[slot].slotTypes =
        std::span<const std::uint8_t>(group.slotTypes.data(), group.slotCount);
    roster.groups[slot].slotFlags =
        std::span<const std::uint8_t>(group.slotFlags.data(), group.slotCount);
    roster.groups[slot].slotIndices =
        std::span<const std::uint16_t>(group.slotIndices.data(), group.slotCount);
    return group.objectTag != 0 && group.registryKey != 0;
}

namespace {

/**
 * Builds the per-bubble sub-blocks from the destination's per-bubble groups.
 * The row holds one bubble mask per group. The wire wants the transpose: one sub-block per
 * bubble, carrying every key that bubble registers.
 * @param bubbleMasks One bubble mask per published per-bubble group, in roster order.
 * @param scratch Lock-owned sub-block storage the spans point into.
 * @param roster Groups already filled, whose per-bubble half starts at the top-level count.
 * @return The sub-blocks to publish, which is empty when the destination has no per-bubble group.
 */
[[nodiscard]] std::span<const message::BubbleSubBlock>
fill_sub_blocks(std::span<const std::uint64_t> bubbleMasks,
                Scratch& scratch,
                const message::Roster& roster) noexcept {
    std::size_t published = 0;
    for (std::size_t bubble = 0; bubble < scratch.rosterSubBlocks.size(); ++bubble) {
        std::size_t keyCount = 0;
        for (std::size_t index = 0; index < bubbleMasks.size(); ++index) {
            if ((bubbleMasks[index] & (std::uint64_t{1} << bubble)) == 0) {
                continue;
            }
            scratch.rosterSubBlockKeys[published][keyCount] =
                roster.groups[roster.topLevelGroupCount + index].key;
            ++keyCount;
        }
        if (keyCount == 0) {
            continue;
        }
        scratch.rosterSubBlocks[published].bubble = static_cast<std::uint32_t>(bubble);
        scratch.rosterSubBlocks[published].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[published].data(), keyCount);
        ++published;
    }
    return std::span(scratch.rosterSubBlocks).first(published);
}

/** @return True when two targets belong to one exact retained roster group. */
[[nodiscard]] bool
same_retained_scope(const RetainedSquadGroup& group,
                    const server::activity::host::ScriptableTarget& target) noexcept {
    const server::activity::host::ScriptableTarget& scope = group.scopeTarget;
    if (scope.stateLocalRoster != target.stateLocalRoster
        || scope.rosterGroupIndex != target.rosterGroupIndex
        || scope.sdkObjectIndex != target.sdkObjectIndex || scope.objectTag != target.objectTag
        || scope.registryKey != target.registryKey || scope.authSchema != target.authSchema
        || scope.stateLocalRegion != target.stateLocalRegion || scope.slotType != target.slotType) {
        return false;
    }
    return !scope.stateLocalRoster
           || (scope.rosterGroupIndex == server::activity::host::kGeneratedRosterGroupIndex
               && scope.sdkObjectIndex != server::activity::host::kNoSdkObjectIndex);
}
} // namespace
/** Logs which exit refused, since the returned outcome itself carries no reason. */
[[nodiscard]] RosterOutcome refuse_override(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=roster_refusal reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return RosterOutcome::noOverrideTarget;
}

/**
 * Finds the full authored SOID for the character the join request named.
 * The client sends a short identity form. Publishing that form binds no object, so the full SOID
 * goes out instead.
 * @param joinCharacter Character id the join request carried, or zero when it carried none.
 * @return Authored SOID of the named character, or of the selected character when nothing matches.
 */
[[nodiscard]] std::uint64_t roster_player_key(std::uint64_t joinCharacter) noexcept {
    const state::AccountState account = state::bound_account_snapshot();
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (joinCharacter == 0) {
        return selected;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const std::uint64_t soid = account.characters[index].soid;
        if ((soid & kIdentityLowMask) == (joinCharacter & kIdentityLowMask)) {
            return soid;
        }
    }
    return selected;
}

/** Compares the used wire fields of two request-owned generated groups. */
[[nodiscard]] bool same_generated_group(const layouts::RosterGroup& left,
                                        const layouts::RosterGroup& right) noexcept {
    if (!layouts::valid_roster_group(left) || !layouts::valid_roster_group(right)
        || left.registryKey != right.registryKey || left.objectTag != right.objectTag
        || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.slotCount; ++index) {
        if (left.slotTypes[index] != right.slotTypes[index]
            || left.slotFlags[index] != right.slotFlags[index]
            || left.slotIndices[index] != right.slotIndices[index]) {
            return false;
        }
    }
    return true;
}

/** Finds one exact same-key group and rejects conflicting or multiply-published keys. */
[[nodiscard]] ExistingGroup find_existing_group(const layouts::RosterGroup& candidate,
                                                const Scratch& scratch,
                                                const message::Roster& roster,
                                                std::size_t& position) noexcept {
    position = roster.groups.size();
    if (!layouts::valid_roster_group(candidate)
        || roster.groupCount > scratch.rosterGroups.size()) {
        return ExistingGroup::conflict;
    }
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        if (scratch.rosterGroups[index].registryKey != candidate.registryKey) {
            continue;
        }
        if (position != roster.groups.size()
            || !same_generated_group(scratch.rosterGroups[index], candidate)) {
            return ExistingGroup::conflict;
        }
        position = index;
    }
    return position == roster.groups.size() ? ExistingGroup::missing : ExistingGroup::exact;
}

/** Ensures a reused non-top-level group is active in the exact requested bubble. */
[[nodiscard]] bool activate_existing_group(std::size_t position,
                                           std::uint32_t bubble,
                                           Scratch& scratch,
                                           message::Roster& roster) noexcept {
    if (position >= roster.groupCount || bubble >= layouts::kBubbleCapacity) {
        return false;
    }
    if (position < roster.topLevelGroupCount) {
        return true;
    }
    std::size_t block = 0;
    while (block < roster.bubbleSubBlocks.size()
           && roster.bubbleSubBlocks[block].bubble != bubble) {
        ++block;
    }
    if (block == roster.bubbleSubBlocks.size()) {
        if (block >= scratch.rosterSubBlocks.size()) {
            return false;
        }
        scratch.rosterSubBlocks[block].bubble = bubble;
        scratch.rosterSubBlockKeys[block][0] = roster.groups[position].key;
        scratch.rosterSubBlocks[block].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), 1);
        roster.bubbleSubBlocks = std::span(scratch.rosterSubBlocks).first(block + 1);
        return true;
    }
    const std::uint32_t key = roster.groups[position].key;
    for (const std::uint32_t existing : roster.bubbleSubBlocks[block].keys) {
        if (existing == key) {
            return true;
        }
    }
    const std::size_t keyCount = roster.bubbleSubBlocks[block].keys.size();
    if (keyCount >= scratch.rosterSubBlockKeys[block].size()) {
        return false;
    }
    scratch.rosterSubBlockKeys[block][keyCount] = key;
    scratch.rosterSubBlocks[block].keys =
        std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), keyCount + 1);
    return true;
}

/**
 * Reads which bubbles one link hosts.
 * One sensor heap serves both activity clients and a roster object costs it once per publishing
 * link, so a bubble's content goes out on one link only.
 * @param session Exact ActivityClient owner of the roster body.
 * @return One bit per hosted bubble, chosen by link kind; every bubble when no world is bound.
 */
[[nodiscard]] std::uint64_t hosted_bubble_mask(const Session& session) noexcept {
    std::uint64_t publicMask = 0;
    if (!region_publicity_mask(session, publicMask)) {
        return ~std::uint64_t{0};
    }
    return session.activity.role == ActivityClientRole::publicTarget ? publicMask : ~publicMask;
}

/**
 * Reports whether one link registers the destination's top-level groups.
 * The client keeps one reference table for all links. A second link that sends the same key
 * rebuilds its objects and repoints the table at copies no body reaches.
 * @param session Exact ActivityClient owner of the roster body.
 * @return False for a public target; its private source link owns those groups.
 */
[[nodiscard]] bool publishes_top_level_groups(const Session& session) noexcept {
    return session.activity.role != ActivityClientRole::publicTarget;
}

/**
 * Copies the destination's published groups into the encoder's fixed input.
 * @param layout Destination row naming its groups by roster table index.
 * @param hostedBubbles One bit per bubble this link hosts; a per-bubble group outside them is
 * left out.
 * @param publishTopLevel False keeps the top-level groups at their positions but retires them, so
 * this link registers none of them.
 * @param scratch Lock-owned roster group storage the spans point into.
 * @param roster Receives the groups and the group that binds the player.
 * @return True when every named group was found and one of them binds the player.
 */
[[nodiscard]] bool fill_roster(const layouts::Definition& layout,
                               std::uint64_t hostedBubbles,
                               bool publishTopLevel,
                               Scratch& scratch,
                               message::Roster& roster) noexcept {
    roster = {};
    const std::size_t groupCount =
        std::size_t{layout.rosterGroupCount} + std::size_t{layout.bubbleGroupCount};
    if (layout.rosterGroupCount == 0 || groupCount > scratch.rosterGroups.size()
        || groupCount > roster.groups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (!fill_group(layout.rosterGroups[index], scratch, index, roster)) {
            return false;
        }
        // Positions stay, so overrides and the mission seed still find the group.
        roster.groups[index].retired = !publishTopLevel;
    }
    // The per-bubble groups follow the top-level ones in the same array. Phase 2 seeds every
    // group the body registers, and the client holds its apply back until they are all in.
    std::array<std::uint64_t, layouts::kDestinationBubbleGroupCapacity> bubbleMasks{};
    std::size_t bubbleGroupCount = 0;
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        const std::uint64_t mask = layout.bubbleGroupMasks[index] & hostedBubbles;
        if (mask == 0) {
            continue;
        }
        if (!fill_group(layout.bubbleGroups[index],
                        scratch,
                        layout.rosterGroupCount + bubbleGroupCount,
                        roster)) {
            return false;
        }
        bubbleMasks[bubbleGroupCount] = mask;
        ++bubbleGroupCount;
    }
    roster.topLevelGroupCount = layout.rosterGroupCount;
    roster.groupCount = std::size_t{layout.rosterGroupCount} + bubbleGroupCount;
    roster.bubbleSubBlocks =
        fill_sub_blocks(std::span(bubbleMasks).first(bubbleGroupCount), scratch, roster);
    // Only a top-level group can bind the player: its object is in every slice set, so the gate
    // reads it wherever the player is.
    for (std::size_t index = 0; index < roster.topLevelGroupCount && roster.playerKeyGroup == 0;
         ++index) {
        const layouts::RosterGroup& group = scratch.rosterGroups[index];
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            if (group.slotTypes[slot] == kSlotTypeParticipation) {
                roster.playerKeyGroup = group.registryKey;
                break;
            }
        }
    }
    return roster.playerKeyGroup != 0;
}

/** Appends one selected-state group and registers its key in its exact authored bubble. */
[[nodiscard]] bool append_state_local_group(const layouts::RosterGroup& generatedGroup,
                                            std::uint32_t bubble,
                                            Scratch& scratch,
                                            message::Roster& roster) noexcept {
    if (bubble >= layouts::kBubbleCapacity || roster.groupCount >= roster.groups.size()) {
        return false;
    }
    const std::size_t groupPosition = roster.groupCount;
    if (!fill_generated_group(generatedGroup, scratch, groupPosition, roster)) {
        return false;
    }
    const std::uint32_t key = roster.groups[groupPosition].key;
    for (std::size_t index = 0; index < groupPosition; ++index) {
        if (roster.groups[index].key == key) {
            return false;
        }
    }
    // A generated group is published the same way whichever path appends it: the mission seed
    // marks its groups seed-only, so an override that appends one first must too.
    roster.groups[groupPosition].missionSeedOnly = true;
    ++roster.groupCount;

    std::size_t block = 0;
    while (block < roster.bubbleSubBlocks.size()
           && roster.bubbleSubBlocks[block].bubble != bubble) {
        ++block;
    }
    if (block == roster.bubbleSubBlocks.size()) {
        if (block >= scratch.rosterSubBlocks.size()) {
            return false;
        }
        scratch.rosterSubBlocks[block].bubble = bubble;
        scratch.rosterSubBlockKeys[block][0] = key;
        scratch.rosterSubBlocks[block].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), 1);
        roster.bubbleSubBlocks = std::span(scratch.rosterSubBlocks).first(block + 1);
        return true;
    }

    const std::size_t keyCount = scratch.rosterSubBlocks[block].keys.size();
    if (keyCount >= scratch.rosterSubBlockKeys[block].size()) {
        return false;
    }
    scratch.rosterSubBlockKeys[block][keyCount] = key;
    scratch.rosterSubBlocks[block].keys =
        std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[block].data(), keyCount + 1);
    return true;
}

/** @return True when one retained entry names this exact slot inside its shared group. */
[[nodiscard]] bool
same_retained_target(const RetainedSquadGroup& group,
                     const RetainedSquadAuth& retained,
                     const server::activity::host::ScriptableTarget& target) noexcept {
    const server::activity::host::ScriptableTarget& scope = group.scopeTarget;
    return scope.objectTag == target.objectTag && scope.registryKey == target.registryKey
           && scope.authSchema == target.authSchema
           && scope.rosterGroupIndex == target.rosterGroupIndex
           && retained.rosterSlotOffset == target.rosterSlotOffset
           && retained.slotIndex == target.slotIndex
           && scope.sdkObjectIndex == target.sdkObjectIndex
           && scope.stateLocalRegion == target.stateLocalRegion && scope.slotType == target.slotType
           && scope.stateLocalRoster == target.stateLocalRoster;
}

/** @return Dense retained-group index for this target, or groupCount when it is new. */
[[nodiscard]] std::size_t
retained_group_index(const SquadOverrideLease& lease,
                     const server::activity::host::ScriptableTarget& target) noexcept {
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        const RetainedSquadGroup& group = lease.groups[index];
        if (same_retained_scope(group, target)) {
            return index;
        }
    }
    return lease.groupCount;
}

/** Copies one already-encoded pending body into the compact retained representation. */
[[nodiscard]] bool
make_retained_squad_auth(const server::activity::host::PendingScriptableOverride& pending,
                         std::uint64_t bindingGeneration,
                         RetainedSquadAuth& output) noexcept {
    output = {};
    if (bindingGeneration == 0 || pending.expectedActivityClientGeneration != bindingGeneration
        || pending.kind != server::activity::host::ScriptableOverrideKind::squad
        || pending.byteCount > output.body.size() || pending.target.slotType != squad::kSlotType
        || pending.target.authSchema != squad::kSchema) {
        return false;
    }
    std::copy_n(pending.body.begin(), pending.byteCount, output.body.begin());
    output.generation = static_cast<std::uint32_t>(pending.generation);
    output.rosterSlotOffset = pending.target.rosterSlotOffset;
    output.slotIndex = pending.target.slotIndex;
    output.bitCount = pending.bitCount;
    output.byteCount = pending.byteCount;
    return true;
}

/** Restores one committed squad body exactly so phase-2 reset cannot clear its slot. */
[[nodiscard]] bool retained_squad_auth(const SquadOverrideLease& lease,
                                       std::size_t index,
                                       message::AuthOverride& output) noexcept {
    output = {};
    if (!lease.active || lease.bindingGeneration == 0 || lease.groupCount == 0
        || lease.groupCount > lease.groups.size() || lease.authCount == 0
        || lease.authCount > lease.authBodies.size() || index >= lease.authCount
        || lease.authBodies[index].groupIndex >= lease.groupCount) {
        return false;
    }
    const RetainedSquadAuth& retained = lease.authBodies[index];
    const RetainedSquadGroup& retainedGroup = lease.groups[retained.groupIndex];
    const server::activity::host::ScriptableTarget& target = retainedGroup.scopeTarget;
    const layouts::RosterGroup& group = retainedGroup.stateLocalRosterGroup;
    if (retained.byteCount > output.body.size() || retainedGroup.authCount == 0
        || target.slotType != squad::kSlotType || target.authSchema != squad::kSchema
        || target.stateLocalRoster != (retainedGroup.region >= 0)
        || target.stateLocalRegion != retainedGroup.region
        || (!target.stateLocalRoster
            && (retained.rosterSlotOffset >= layouts::kRosterSlotCapacity
                || retained.slotIndex > message::kMaximumSlotIndex))
        || (target.stateLocalRoster
            && (!layouts::valid_roster_group(group)
                || target.rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
                || target.sdkObjectIndex == server::activity::host::kNoSdkObjectIndex
                || group.objectTag != target.objectTag || group.registryKey != target.registryKey
                || retained.rosterSlotOffset >= group.slotCount
                || group.slotTypes[retained.rosterSlotOffset] != target.slotType
                || group.slotIndices[retained.rosterSlotOffset] != retained.slotIndex
                || (group.slotFlags[retained.rosterSlotOffset] & message::kSlotAuthFlag) == 0))) {
        return false;
    }
    std::copy_n(retained.body.begin(), retained.byteCount, output.body.begin());
    output.objectTag = target.objectTag;
    output.key = target.registryKey;
    output.authSchema = target.authSchema;
    output.slotIndex = retained.slotIndex;
    output.bitCount = retained.bitCount;
    output.slotType = target.slotType;
    output.byteCount = retained.byteCount;
    output.present = true;
    return true;
}

/** @return True when every retained group and body is safe for cumulative publication. */
[[nodiscard]] bool valid_retained_squad_lease(const SquadOverrideLease& lease,
                                              std::uint64_t bindingGeneration) noexcept {
    if (!lease.active || bindingGeneration == 0 || lease.bindingGeneration != bindingGeneration
        || lease.groupCount == 0 || lease.groupCount > lease.groups.size() || lease.authCount == 0
        || lease.authCount > lease.authBodies.size()) {
        return false;
    }
    std::array<std::uint16_t, message::kPublishedGroupCapacity> authCounts{};
    for (std::size_t groupIndex = 0; groupIndex < lease.groupCount; ++groupIndex) {
        const RetainedSquadGroup& group = lease.groups[groupIndex];
        const server::activity::host::ScriptableTarget& target = group.scopeTarget;
        if (group.authCount == 0 || group.stateSequence > message::kMaximumStateSequence
            || target.slotType != squad::kSlotType || target.authSchema != squad::kSchema
            || target.stateLocalRoster != (group.region >= 0)
            || target.stateLocalRegion != group.region
            || (target.stateLocalRoster
                && (!layouts::valid_roster_group(group.stateLocalRosterGroup)
                    || group.authCount > group.stateLocalRosterGroup.slotCount
                    || target.rosterGroupIndex != server::activity::host::kGeneratedRosterGroupIndex
                    || target.sdkObjectIndex == server::activity::host::kNoSdkObjectIndex
                    || group.stateLocalRosterGroup.objectTag != target.objectTag
                    || group.stateLocalRosterGroup.registryKey != target.registryKey))) {
            return false;
        }
        for (std::size_t other = 0; other < groupIndex; ++other) {
            if (lease.groups[other].scopeTarget.registryKey == target.registryKey) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < lease.authCount; ++index) {
        message::AuthOverride value{};
        if (!retained_squad_auth(lease, index, value)) {
            return false;
        }
        ++authCounts[lease.authBodies[index].groupIndex];
    }
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        if (authCounts[index] != lease.groups[index].authCount) {
            return false;
        }
    }
    return true;
}

/** @return Whether one canonical group is registered in the selected bubble. */
[[nodiscard]] CanonicalGroupStatus canonical_group_status(const layouts::Definition& layout,
                                                          const EffectiveRegion& region,
                                                          std::uint16_t tableIndex) noexcept {
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (layout.rosterGroups[index] == tableIndex) {
            return CanonicalGroupStatus::active;
        }
    }
    bool found = false;
    const std::uint32_t bubble =
        region.index >= 0 ? static_cast<std::uint32_t>(region.index)
                                / middleware::content::packages::tables::kSliceSetIndexFactor
                          : layouts::kBubbleCapacity;
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        if (layout.bubbleGroups[index] != tableIndex) {
            continue;
        }
        found = true;
        if (bubble < layouts::kBubbleCapacity
            && (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << bubble)) != 0) {
            return CanonicalGroupStatus::active;
        }
    }
    return found ? CanonicalGroupStatus::inactive : CanonicalGroupStatus::unknown;
}

/** Installs an override only when its canonical roster row is registered in this exact bubble. */
[[nodiscard]] bool install_auth_override(const layouts::Definition& layout,
                                         const EffectiveRegion& region,
                                         Scratch& scratch,
                                         message::Snapshot& snapshot,
                                         const message::AuthOverride& value,
                                         std::uint16_t tableIndex,
                                         std::uint16_t slotOffset,
                                         bool stateLocalRosterTarget) noexcept {
    std::size_t rosterPosition = snapshot.roster.groupCount;
    if (stateLocalRosterTarget) {
        for (std::size_t index = 0; index < snapshot.roster.groupCount; ++index) {
            const message::Group& group = snapshot.roster.groups[index];
            if (group.key != value.key || group.objectTag != value.objectTag) {
                continue;
            }
            if (rosterPosition != snapshot.roster.groupCount) {
                return false;
            }
            rosterPosition = index;
        }
    }
    for (std::size_t index = 0; !stateLocalRosterTarget && index < layout.rosterGroupCount;
         ++index) {
        if (layout.rosterGroups[index] == tableIndex) {
            if (rosterPosition != snapshot.roster.groupCount) {
                return false;
            }
            rosterPosition = index;
        }
    }
    const std::uint32_t bubble =
        region.index >= 0 ? static_cast<std::uint32_t>(region.index)
                                / middleware::content::packages::tables::kSliceSetIndexFactor
                          : layouts::kBubbleCapacity;
    for (std::size_t index = 0; !stateLocalRosterTarget && index < layout.bubbleGroupCount;
         ++index) {
        if (layout.bubbleGroups[index] != tableIndex || bubble >= layouts::kBubbleCapacity
            || (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << bubble)) == 0) {
            continue;
        }
        if (rosterPosition != snapshot.roster.groupCount) {
            return false;
        }
        // Unhosted bubble groups are left out of the roster, so the position comes from the key.
        for (std::size_t row = snapshot.roster.topLevelGroupCount; row < snapshot.roster.groupCount;
             ++row) {
            if (snapshot.roster.groups[row].key == value.key) {
                rosterPosition = row;
                break;
            }
        }
    }
    if (rosterPosition >= snapshot.roster.groupCount) {
        return false;
    }
    const layouts::RosterGroup& group = scratch.rosterGroups[rosterPosition];
    if (slotOffset >= group.slotCount || group.objectTag != value.objectTag
        || group.registryKey != value.key || group.slotTypes[slotOffset] != value.slotType
        || group.slotIndices[slotOffset] != value.slotIndex
        || (group.slotFlags[slotOffset] & message::kSlotAuthFlag) == 0) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.authOverrides.size(); ++index) {
        const message::AuthOverride& retained = snapshot.authOverrides[index];
        if (retained.objectTag == value.objectTag && retained.key == value.key
            && retained.slotIndex == value.slotIndex && retained.slotType == value.slotType) {
            scratch.rosterAuthOverrides[index] = value;
            return true;
        }
    }
    const std::size_t overrideCount = snapshot.authOverrides.size();
    if (overrideCount >= scratch.rosterAuthOverrides.size()) {
        return false;
    }
    scratch.rosterAuthOverrides[overrideCount] = value;
    snapshot.authOverrides = std::span(scratch.rosterAuthOverrides).first(overrideCount + 1);
    return true;
}

/** Copies one delivered Host body into the message codec's value type. */
[[nodiscard]] bool
make_auth_override(const server::activity::host::PendingScriptableOverride& retained,
                   message::AuthOverride& output) noexcept {
    output = {};
    if (retained.kind == server::activity::host::ScriptableOverrideKind::lifetime
        || retained.byteCount == 0 || retained.byteCount > retained.body.size()
        || retained.byteCount > output.body.size()) {
        return false;
    }
    std::copy_n(retained.body.begin(), retained.byteCount, output.body.begin());
    output.objectTag = retained.target.objectTag;
    output.key = retained.target.registryKey;
    output.authSchema = retained.target.authSchema;
    output.slotIndex = retained.target.slotIndex;
    output.bitCount = retained.bitCount;
    output.slotType = retained.target.slotType;
    output.byteCount = retained.byteCount;
    output.sdkCompiled = retained.sdkCompiled;
    output.present = true;
    output.originatingHostRevision = retained.revision;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::activity
