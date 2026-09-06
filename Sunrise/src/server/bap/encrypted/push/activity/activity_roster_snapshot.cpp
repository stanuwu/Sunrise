#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../../state/account/account_state.h"
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
namespace {

namespace layouts = state::build_data::scenarios;
namespace scriptable = middleware::bap::activity_message::scriptable_auth;
namespace squad = middleware::bap::activity_message::squad_auth;

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

/** The state byte is stored biased into one signed byte, so the sequence stays inside this. */
constexpr std::uint8_t kStateSequenceWrap = 128;
/** Standard 32-bit FNV-1a basis and prime fold the group set into one comparable value. */
constexpr std::uint32_t kFoldBasis = 2166136261U;
constexpr std::uint32_t kFoldPrime = 16777619U;
/** Only a type-13 slot binds the player, so only a group holding one may carry the key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** The join request names its character in the low half of the SOID, so compare on that half. */
constexpr std::uint64_t kIdentityLowMask = 0xFFFFFFFFULL;

/**
 * Finds the full authored SOID for the character the join request named.
 * The client sends a short identity form. Publishing that form binds no object, so the full SOID
 * goes out instead.
 * @param joinCharacter Character id the join request carried, or zero when it carried none.
 * @return Authored SOID of the named character, or of the selected character when nothing matches.
 */
[[nodiscard]] std::uint64_t roster_player_key(std::uint64_t joinCharacter) noexcept {
    const state::AccountState account = state::account_snapshot();
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

/** Result of joining a generated group against every group already staged in this roster. */
enum class ExistingGroup : std::uint8_t {
    missing,
    exact,
    conflict,
};

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
 * Copies the destination's published groups into the encoder's fixed input.
 * @param layout Destination row naming its groups by roster table index.
 * @param hostedBubbles One bit per bubble this link hosts; a per-bubble group outside them is
 * left out.
 * @param scratch Lock-owned roster group storage the spans point into.
 * @param roster Receives the groups and the group that binds the player.
 * @return True when every named group was found and one of them binds the player.
 */
[[nodiscard]] bool fill_roster(const layouts::Definition& layout,
                               std::uint64_t hostedBubbles,
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

enum class CanonicalGroupStatus : std::uint8_t {
    unknown,
    inactive,
    active,
};

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
        rosterPosition = layout.rosterGroupCount + index;
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
    return true;
}

/** @return FNV-1a over one group's registration identity: its key, tag, slot set and epoch. */
[[nodiscard]] std::uint32_t group_identity_fold(const message::Group& group,
                                                std::uint8_t regionEpoch) noexcept {
    std::uint32_t folded = kFoldBasis;
    const auto mix = [&folded](std::uint32_t value) noexcept {
        folded = (folded ^ value) * kFoldPrime;
    };
    // A region transition advances the epoch, which moves every group's fold and so its wire byte,
    // and the client re-registers each group for the region it entered. Without the epoch it keeps
    // the previous region's registration and every object intent into the new bubble is refused.
    mix(regionEpoch);
    mix(group.key);
    mix(group.objectTag);
    mix(static_cast<std::uint32_t>(group.slotTypes.size()));
    for (const std::uint8_t slotType : group.slotTypes) {
        mix(slotType);
    }
    for (const std::uint8_t slotFlag : group.slotFlags) {
        mix(slotFlag);
    }
    for (const std::uint16_t slotIndex : group.slotIndices) {
        mix(slotIndex);
    }
    // The seed-only flag selects phase-2 content, not what the client registers. A moved byte
    // rebuilds the group and stops a cutscene playing in it, so content never moves the byte.
    return folded;
}

/**
 * Advances the epoch once per bubble the client holds, so every group re-registers there.
 * @param refresh Refresh being answered, which stands in for its uncommitted bubble, or null.
 */
void advance_region_epoch(Session& session, const RefreshReport* refresh) noexcept {
    const std::int32_t held =
        state::activity::membership::instantiated_region(client_placement(session, refresh));
    if (held < 0) {
        return;
    }
    const std::int32_t bubble =
        held
        / static_cast<std::int32_t>(middleware::content::packages::tables::kSliceSetIndexFactor);
    if (bubble == session.activityRosterRegionBubble) {
        return;
    }
    const std::int32_t previous = session.activityRosterRegionBubble;
    // The first bubble registers every group as a new key, so it needs no move.
    if (previous >= 0) {
        session.activityRosterRegionEpoch =
            static_cast<std::uint8_t>(session.activityRosterRegionEpoch + 1U);
    }
    session.activityRosterRegionBubble = bubble;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=roster_state result=region_moved "
                                      "bubble=%d from=%d region=%d epoch=%u",
                                      bubble,
                                      previous,
                                      held,
                                      static_cast<unsigned>(session.activityRosterRegionEpoch));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Stamps every group's revision from its lease: stable while the group's identity holds,
 * advanced only for a new key or a changed identity. The wire carries one byte per key, and the
 * client rebuilds only the groups whose byte moved, so an unrelated change replays nothing.
 */
void stamp_group_sequences(Session& session, message::Roster& roster) noexcept {
    static_assert(kRosterGroupLeaseCapacity >= message::kPublishedGroupCapacity);
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        message::Group& group = roster.groups[index];
        const std::uint32_t folded = group_identity_fold(group, session.activityRosterRegionEpoch);
        RosterGroupLease* lease = nullptr;
        RosterGroupLease* freeSlot = nullptr;
        for (RosterGroupLease& candidate : session.activityRosterGroupLeases) {
            if (candidate.used && candidate.key == group.key) {
                lease = &candidate;
                break;
            }
            if (!candidate.used && freeSlot == nullptr) {
                freeSlot = &candidate;
            }
        }
        const bool newKey = lease == nullptr;
        if (newKey || lease->identityFold != folded) {
            session.activityRosterState =
                static_cast<std::uint8_t>((session.activityRosterState + 1) % kStateSequenceWrap);
            if (lease == nullptr) {
                lease = freeSlot;
            }
            if (lease != nullptr) {
                lease->key = group.key;
                lease->identityFold = folded;
                lease->sequence = session.activityRosterState;
                lease->used = true;
            }
            std::array<char, core::log::kLineCapacity> line{};
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=activity stage=roster_state result=group_moved key=0x%08X "
                              "seq=%u new=%d",
                              group.key,
                              session.activityRosterState,
                              newKey ? 1 : 0);
            if (written > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::debug,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        group.stateSequence = lease != nullptr ? lease->sequence : session.activityRosterState;
        group.hasStateSequence = true;
    }
}

} // namespace

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
    // WS-702 world-state 8 follows the bootflow's arrival, independently of the player spawn.
    // Holding a region alone can precede that report and the world-transition fade's final arm.
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
    bool pendingInstalled = false;
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
        pendingInstalled = pendingInstalled || replace;
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
    if (authOverride != nullptr && !pendingInstalled
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
    // Staging runs before the connection field is published, so a body answering message 52 has to
    // take the epoch from that message instead of from the connection.
    snapshot.patchEpoch = epoch != nullptr ? *epoch : session.activityPatchEpoch.value;
    // The character the join named wins, resolved to its authored SOID. The client binds its
    // player by matching this value against the object registry, and the short form the join
    // carries matches nothing.
    snapshot.playerKey = published_player_key(session);
    snapshot.lifetime = lifetimeState;
    // Keep the native spawn gate held until the client's arrival report. A region can be loaded
    // before bootflow finishes and arms its fade; spawning then releases an inactive fade and
    // leaves the later black overlay stuck. Arrival is independent of spawning, so this hold
    // releases on WS-702 world-state 8 without a client patch or a load-duration timeout.
    snapshot.awaitClientSync = !client_in_world(session, refresh);
    // Player_BindComponents walks every type-13 reference and the player datum can name any one of
    // them. So every participation record carries the same player key. Selecting the first slot
    // leaves the authored cinematic participant unbound whenever it names another record.
    snapshot.keyOnEveryParticipationSlot = true;
    snapshot.authorDirectorBodies = defaults.authorDirectorBodies;
    snapshot.authorWideRecordBodies = defaults.authorWideRecordBodies;
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
