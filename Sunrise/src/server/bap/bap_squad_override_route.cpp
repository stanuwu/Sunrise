/** Squad Auth overrides: the retained per-binding squad set and the capacities it must fit. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

#include "../../state/build_data/runtime.h"
#include "../activity/host_runtime.h"
#include "internal.h"
#include "runtime.h"
#include "squad_override_capacity.h"

namespace sunrise::server::bap {
namespace {

namespace layouts = state::build_data::scenarios;
namespace roster_message = middleware::bap::activity_message::sensor_auth_update;
namespace tables = middleware::content::packages::tables;

/** @return True when two values name the same full ClientRef slot. */
[[nodiscard]] bool same_scriptable_target(const activity::host::ScriptableTarget& left,
                                          const activity::host::ScriptableTarget& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.authSchema == right.authSchema && left.rosterGroupIndex == right.rosterGroupIndex
           && left.rosterSlotOffset == right.rosterSlotOffset && left.slotIndex == right.slotIndex
           && left.sdkObjectIndex == right.sdkObjectIndex
           && left.stateLocalRegion == right.stateLocalRegion && left.slotType == right.slotType
           && left.stateLocalRoster == right.stateLocalRoster;
}

/** @return True when two squad slots share one exact retained roster group. */
[[nodiscard]] bool same_squad_scope(const activity::host::ScriptableTarget& left,
                                    const activity::host::ScriptableTarget& right) noexcept {
    if (left.stateLocalRoster != right.stateLocalRoster
        || left.rosterGroupIndex != right.rosterGroupIndex
        || left.sdkObjectIndex != right.sdkObjectIndex || left.objectTag != right.objectTag
        || left.registryKey != right.registryKey || left.authSchema != right.authSchema
        || left.stateLocalRegion != right.stateLocalRegion || left.slotType != right.slotType) {
        return false;
    }
    return !left.stateLocalRoster
           || (left.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
               && left.sdkObjectIndex != activity::host::kNoSdkObjectIndex);
}

/** @return Dense retained-group index for this target, or groupCount when it is new. */
[[nodiscard]] std::size_t
retained_squad_group(const SquadOverrideLease& lease,
                     const activity::host::ScriptableTarget& target) noexcept {
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        const RetainedSquadGroup& group = lease.groups[index];
        if (same_squad_scope(group.scopeTarget, target)) {
            return index;
        }
    }
    return lease.groupCount;
}

/** @return True when one compact retained body names the exact requested slot. */
[[nodiscard]] bool retained_squad_target(const SquadOverrideLease& lease,
                                         std::size_t groupIndex,
                                         const activity::host::ScriptableTarget& target) noexcept {
    if (!lease.active || groupIndex >= lease.groupCount || lease.authCount == 0
        || lease.authCount > lease.authBodies.size()) {
        return false;
    }
    const RetainedSquadGroup& group = lease.groups[groupIndex];
    for (std::size_t index = 0; index < lease.authCount; ++index) {
        const RetainedSquadAuth& body = lease.authBodies[index];
        if (body.groupIndex != groupIndex) {
            continue;
        }
        activity::host::ScriptableTarget retained = group.scopeTarget;
        retained.rosterSlotOffset = body.rosterSlotOffset;
        retained.slotIndex = body.slotIndex;
        if (same_scriptable_target(retained, target)) {
            return true;
        }
    }
    return false;
}

/** @return True when two request-owned groups carry the same complete wire layout. */
[[nodiscard]] bool
same_scriptable_group(const state::build_data::scenarios::RosterGroup& left,
                      const state::build_data::scenarios::RosterGroup& right) noexcept {
    if (!state::build_data::scenarios::valid_roster_group(left)
        || !state::build_data::scenarios::valid_roster_group(right) || left.objectTag == 0
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

/** @return True when a generated group owns the exact requested type-1 slot. */
[[nodiscard]] bool valid_state_local_squad_target(const activity::host::ScriptableTarget& target,
                                                  const layouts::RosterGroup* group) noexcept {
    return target.stateLocalRoster && group != nullptr && layouts::valid_roster_group(*group)
           && target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
           && group->objectTag == target.objectTag && group->registryKey == target.registryKey
           && target.rosterSlotOffset < group->slotCount
           && group->slotTypes[target.rosterSlotOffset]
                  == middleware::bap::activity_message::squad_auth::kSlotType
           && group->slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group->slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

/** @return True when the dense retained set has valid counts and exact group ownership. */
[[nodiscard]] bool valid_squad_override_lease(const SquadOverrideLease& lease,
                                              std::uint64_t bindingGeneration) noexcept {
    if (!lease.active || bindingGeneration == 0 || lease.bindingGeneration != bindingGeneration
        || lease.groupCount == 0 || lease.groupCount > lease.groups.size() || lease.authCount == 0
        || lease.authCount > lease.authBodies.size()) {
        return false;
    }
    std::array<std::uint16_t, roster_message::kPublishedGroupCapacity> authCounts{};
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        const RetainedSquadGroup& group = lease.groups[index];
        const activity::host::ScriptableTarget& target = group.scopeTarget;
        if (group.authCount == 0 || group.stateSequence > roster_message::kMaximumStateSequence
            || target.slotType != middleware::bap::activity_message::squad_auth::kSlotType
            || target.authSchema != middleware::bap::activity_message::squad_auth::kSchema
            || target.stateLocalRoster != (group.region >= 0)
            || target.stateLocalRegion != group.region
            || (target.stateLocalRoster
                && (group.authCount > group.stateLocalRosterGroup.slotCount
                    || !valid_state_local_squad_target(target, &group.stateLocalRosterGroup)))) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (lease.groups[other].scopeTarget.registryKey == target.registryKey) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < lease.authCount; ++index) {
        const RetainedSquadAuth& body = lease.authBodies[index];
        if (body.groupIndex >= lease.groupCount) {
            return false;
        }
        ++authCounts[body.groupIndex];
    }
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        if (authCounts[index] != lease.groups[index].authCount) {
            return false;
        }
    }
    return true;
}

/** @return True when a generated key does not alias a canonical destination group. */
[[nodiscard]] bool generated_key_is_unique(const layouts::Definition& layout,
                                           std::uint32_t registryKey) noexcept {
    const std::size_t topCount = layout.rosterGroupCount;
    const std::size_t bubbleCount = layout.bubbleGroupCount;
    if (topCount > layout.rosterGroups.size() || bubbleCount > layout.bubbleGroups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < topCount + bubbleCount; ++index) {
        const std::uint16_t tableIndex =
            index < topCount ? layout.rosterGroups[index] : layout.bubbleGroups[index - topCount];
        layouts::RosterGroup group{};
        if (!state::build_data::find_roster_group(tableIndex, group)
            || group.registryKey == registryKey) {
            return false;
        }
    }
    return true;
}

/** @return True when one more generated key fits the target bubble's nested key array. */
[[nodiscard]] bool generated_bubble_has_capacity(const layouts::Definition& layout,
                                                 const SquadOverrideLease* lease,
                                                 std::int32_t region) noexcept {
    if (region < 0 || layout.bubbleGroupCount > layout.bubbleGroupMasks.size()) {
        return false;
    }
    const std::uint32_t bubble = static_cast<std::uint32_t>(region) / tables::kSliceSetIndexFactor;
    if (bubble >= layouts::kBubbleCapacity) {
        return false;
    }
    std::size_t keys = 0;
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        keys += (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << bubble)) != 0 ? 1U : 0U;
    }
    for (std::size_t index = 0; lease != nullptr && index < lease->groupCount; ++index) {
        const RetainedSquadGroup& group = lease->groups[index];
        if (group.scopeTarget.stateLocalRoster
            && static_cast<std::uint32_t>(group.region) / tables::kSliceSetIndexFactor == bubble) {
            ++keys;
        }
    }
    return keys < roster_message::kBubbleKeyCapacity;
}

/** Checks one squad request against exact retained and message-5 capacities. */
[[nodiscard]] bool
squad_override_available_locked(const Session& session,
                                const activity::host::ScriptableTarget& target,
                                const layouts::RosterGroup* stateLocalRosterGroup,
                                std::uint64_t expectedGeneration) noexcept {
    if (expectedGeneration == 0 || session.activity.bindingGeneration != expectedGeneration
        || target.slotType != middleware::bap::activity_message::squad_auth::kSlotType
        || target.authSchema != middleware::bap::activity_message::squad_auth::kSchema
        || (target.stateLocalRoster
            && !valid_state_local_squad_target(target, stateLocalRosterGroup))
        || (!target.stateLocalRoster
            && (stateLocalRosterGroup != nullptr || target.stateLocalRegion >= 0))) {
        return false;
    }
    layouts::Definition layout{};
    if (!session_scenario_layout(session, layout)) {
        return false;
    }
    const std::size_t canonicalGroups =
        std::size_t{layout.rosterGroupCount} + std::size_t{layout.bubbleGroupCount};
    if (canonicalGroups == 0 || canonicalGroups > roster_message::kPublishedGroupCapacity) {
        return false;
    }
    const std::int32_t selectedRegion = selected_region_index_locked(session);
    const SquadOverrideLease& lease = session.activitySquadOverride;
    if (!lease.active) {
        const layouts::RosterGroup* const pendingGroup =
            target.stateLocalRoster ? stateLocalRosterGroup : nullptr;
        if (!squad_override_capacity::available(layout,
                                                nullptr,
                                                pendingGroup,
                                                target.stateLocalRoster,
                                                target.stateLocalRegion,
                                                selectedRegion)) {
            return false;
        }
        if (!target.stateLocalRoster) {
            return true;
        }
        return canonicalGroups < roster_message::kPublishedGroupCapacity
               && generated_key_is_unique(layout, target.registryKey)
               && generated_bubble_has_capacity(layout, nullptr, target.stateLocalRegion);
    }
    if (!valid_squad_override_lease(lease, expectedGeneration)) {
        return false;
    }
    std::size_t stateLocalGroups = 0;
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        const RetainedSquadGroup& group = lease.groups[index];
        if (group.scopeTarget.stateLocalRoster) {
            ++stateLocalGroups;
        }
    }
    if (canonicalGroups + stateLocalGroups > roster_message::kPublishedGroupCapacity) {
        return false;
    }
    const std::size_t groupIndex = retained_squad_group(lease, target);
    const layouts::RosterGroup* const pendingGroup =
        target.stateLocalRoster && groupIndex == lease.groupCount ? stateLocalRosterGroup : nullptr;
    if (!squad_override_capacity::available(layout,
                                            &lease,
                                            pendingGroup,
                                            target.stateLocalRoster,
                                            target.stateLocalRegion,
                                            selectedRegion)) {
        return false;
    }
    if (groupIndex < lease.groupCount) {
        const RetainedSquadGroup& group = lease.groups[groupIndex];
        if (target.stateLocalRoster
            && !same_scriptable_group(group.stateLocalRosterGroup, *stateLocalRosterGroup)) {
            return false;
        }
        if (retained_squad_target(lease, groupIndex, target)) {
            return true;
        }
        return lease.authCount < lease.authBodies.size()
               && (!target.stateLocalRoster
                   || group.authCount < group.stateLocalRosterGroup.slotCount);
    }
    if (lease.authCount >= lease.authBodies.size()) {
        return false;
    }
    if (!target.stateLocalRoster) {
        return lease.groupCount < lease.groups.size();
    }
    if (stateLocalGroups != lease.groupCount
        || canonicalGroups + stateLocalGroups >= roster_message::kPublishedGroupCapacity
        || !generated_key_is_unique(layout, target.registryKey)) {
        return false;
    }
    if (!generated_bubble_has_capacity(layout, &lease, target.stateLocalRegion)) {
        return false;
    }
    for (std::size_t index = 0; index < lease.groupCount; ++index) {
        const RetainedSquadGroup& group = lease.groups[index];
        if (group.scopeTarget.registryKey == target.registryKey) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Queues a squad placement only while the requested authenticated client generation owns the
 * binding. */
bool request_activity_squad_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::int32_t> requestedCounts,
    middleware::bap::activity_message::squad_auth::Mode mode,
    std::optional<std::uint32_t> nameHash,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation,
    std::array<std::int8_t, 4> authoredProfile,
    state::gameplay::squad_entity_retirement::Eligibility squadRetirement,
    std::optional<middleware::bap::activity_message::squad_auth::Destination> destination,
    std::optional<middleware::bap::activity_message::squad_auth::SpawnRule> spawnRule) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session =
        activity_link_for_generation_locked(binding, expectedGeneration, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued = expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
                        && session->activity.bindingGeneration == expectedGeneration
                        && squad_override_available_locked(
                            *session, target, stateLocalRosterGroup, expectedGeneration)
                        && region == expectedRegion
                        && activity::host::request_squad_override(binding,
                                                                  target,
                                                                  stateLocalRosterGroup,
                                                                  requestedCounts,
                                                                  mode,
                                                                  expectedGeneration,
                                                                  nameHash,
                                                                  reservation,
                                                                  authoredProfile,
                                                                  squadRetirement,
                                                                  destination,
                                                                  spawnRule);
    return queued;
}

} // namespace sunrise::server::bap
