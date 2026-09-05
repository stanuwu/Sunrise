#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string_view>

#include "../../client/hooks/network/investment/investment_derived_rebuild.h"
#include "../../core/logging/log.h"
#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../state/activity/runtime.h"
#include "../../state/build_data/runtime.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "../../state/progression/seasonal_experience.h"
#include "../activity/host_runtime.h"
#include "activity_authority_query_owner.h"
#include "activity_host_selection.h"
#include "activity_authority_reset_owner.h"
#include "activity_mission_seed_lease.h"
#include "core/threading/srw_lock.h"
#include "encrypted/bap_connection_publication.h"
#include "encrypted/push/activity/internal.h"
#include "internal.h"
#include "runtime.h"
#include "squad_override_capacity.h"

namespace sunrise::server::bap {
namespace {

namespace layouts = state::build_data::scenarios;
namespace roster_message = middleware::bap::activity_message::sensor_auth_update;
namespace tables = middleware::content::packages::tables;

core::threading::SrwLock g_lock{};
std::array<Session, kSessionCount> g_sessions{};
Scratch g_scratch{};
std::array<WorldRewardRequest, kWorldRewardQueueCapacity> g_worldRewards{};
std::size_t g_worldRewardHead{};
std::size_t g_worldRewardCount{};
/** Measured lifetime of the native item-acquisition flyout. */
constexpr std::uint64_t kAcquisitionPresentationHoldMs = 8'000;
constexpr std::uint8_t kWorldRewardFailureLimit = 8;

[[nodiscard]] bool has_active_family4_peer() noexcept {
    return std::any_of(g_sessions.begin(), g_sessions.end(), [](const Session& session) {
        return session.id != 0 && session.authenticated && session.queuez.family4Active;
    });
}

void pop_world_reward() noexcept {
    g_worldRewards[g_worldRewardHead] = {};
    g_worldRewardHead = (g_worldRewardHead + 1) % g_worldRewards.size();
    --g_worldRewardCount;
}

[[nodiscard]] bool commit_world_reward(const WorldRewardRequest& request) noexcept {
    if (request.kind == WorldRewardKind::item) {
        state::PendingItemAcquisition acquisition{};
        return state::prepare_item_acquisition_for_item(request.itemDefinitionIndex, acquisition)
               && state::commit_item_acquisition(acquisition);
    }
    state::PendingProfileItemAcquisition acquisition{};
    return state::prepare_profile_item_acquisition_for_item(
               request.itemDefinitionIndex, request.quantity, acquisition)
           && state::commit_profile_item_acquisition(acquisition);
}

[[nodiscard]] bool enqueue_world_reward(WorldRewardRequest request) noexcept {
    if (!has_active_family4_peer()) {
        while (g_worldRewardCount != 0) {
            if (!commit_world_reward(g_worldRewards[g_worldRewardHead])) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=world_reward stage=direct result=drop");
            }
            pop_world_reward();
        }
        return commit_world_reward(request);
    }
    if (g_worldRewardCount == g_worldRewards.size()) {
        const bool committed = commit_world_reward(g_worldRewards[g_worldRewardHead]);
        if (committed) {
            arm_account_resync_everywhere();
        }
        pop_world_reward();
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         committed ? "ev=world_reward stage=queue_full result=direct"
                                   : "ev=world_reward stage=queue_full result=drop");
    }
    g_worldRewards[(g_worldRewardHead + g_worldRewardCount) % g_worldRewards.size()] = request;
    ++g_worldRewardCount;
    return true;
}

/** Arms every other active peer after one shared-account transaction is published. */
void publish_account_mutation(Session& origin) noexcept {
    origin.accountMutationPublished = false;
    arm_account_resync_elsewhere(origin);
}
/** @param id Nonzero connection id. @return Matching open session, or null. */
[[nodiscard]] Session* session_for(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return nullptr;
    }
    auto& session = g_sessions[id - 1];
    return session.id == id ? &session : nullptr;
}

/** @return True when every scalar and captured destination field is identical. */
[[nodiscard]] bool
same_destination(const state::activity::destination::DestinationSelection& left,
                 const state::activity::destination::DestinationSelection& right) noexcept {
    return left.packageName == right.packageName
           && left.packageNameLength == right.packageNameLength && left.reason == right.reason
           && left.sourceActivityIndex == right.sourceActivityIndex
           && left.activityIndex == right.activityIndex && left.elementIndex == right.elementIndex
           && left.selectionNonce == right.selectionNonce
           && left.arrivalBubbleHash == right.arrivalBubbleHash
           && left.spawnSetHash == right.spawnSetHash
           && left.hasElementIndex == right.hasElementIndex
           && left.hasSelectionNonce == right.hasSelectionNonce
           && left.hasArrivalBubbleHash == right.hasArrivalBubbleHash
           && left.hasSpawnSetHash == right.hasSpawnSetHash
           && left.arrivalBubbleOverride == right.arrivalBubbleOverride
           && left.hasArrivalBubbleOverride == right.hasArrivalBubbleOverride
           && left.sliceSetOverride == right.sliceSetOverride
           && left.hasSliceSetOverride == right.hasSliceSetOverride
           && left.spawnSetOverride == right.spawnSetOverride
           && left.hasSpawnSetOverride == right.hasSpawnSetOverride
           && left.descriptorBits == right.descriptorBits
           && left.descriptorBitLength == right.descriptorBitLength
           && left.descriptorNameBit == right.descriptorNameBit
           && left.hasDescriptorName == right.hasDescriptorName;
}

/**
 * Tests the world-package identity used by the HUD and generated scenario layout.
 * Public targets are separate selections and may legitimately carry a different reason, nonce,
 * activity index or descriptor while still naming the same loaded destination. Those fields are
 * binding identity, not evidence that two ActivityClients belong to different maps.
 */
[[nodiscard]] bool
same_destination_package(const state::activity::destination::DestinationSelection& left,
                         const state::activity::destination::DestinationSelection& right) noexcept {
    return left.packageNameLength != 0 && left.packageNameLength == right.packageNameLength
           && left.packageName == right.packageName;
}

/** Finds one exact authenticated ActivityClient while the caller owns the BAP lock. */
[[nodiscard]] const Session*
unique_activity_link_locked(const state::activity::SessionBinding& binding,
                            std::size_t& count) noexcept {
    count = 0;
    const Session* selected = nullptr;
    for (const Session& session : g_sessions) {
        const auto& owned = session.activity.session;
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0 || owned.sessionId != binding.sessionId
            || owned.createdRevision != binding.createdRevision
            || !same_destination(owned.destination, binding.destination)) {
            continue;
        }
        selected = &session;
        ++count;
    }
    return count == 1 ? selected : nullptr;
}

/** Returns the mutable unique-link result while the caller owns the exclusive BAP lock. */
[[nodiscard]] Session*
unique_mutable_activity_link_locked(const state::activity::SessionBinding& binding,
                                    std::size_t& count) noexcept {
    return const_cast<Session*>(unique_activity_link_locked(binding, count));
}

/** Validates and, when stale, clears one connection-owned SDK selected-state roster lease. */
[[nodiscard]] ActivityMissionSeedLeaseStatus
mission_seed_link_locked(const state::activity::SessionBinding& binding,
                         std::uint32_t scenarioRow,
                         std::uint64_t expectedGeneration,
                         Session*& output,
                         std::size_t& matchingLinks) noexcept {
    output = unique_mutable_activity_link_locked(binding, matchingLinks);
    if (output == nullptr) {
        return ActivityMissionSeedLeaseStatus::noActivityLink;
    }
    return mission_seed_session_status(*output, scenarioRow, expectedGeneration);
}

/** Resolves the region used by this exact connection's msg-5 builder. */
[[nodiscard]] encrypted::push::activity::EffectiveRegion
selected_region_locked(const Session& session) noexcept {
    const auto base = encrypted::push::activity::effective_region(session.activity.session);
    return encrypted::push::activity::selected_effective_region(session, base.arrival);
}

/** @return True when two leases name the same complete generated plan. */
[[nodiscard]] bool same_mission_seed_plan(const ActivityMissionSeedPlan& left,
                                          const ActivityMissionSeedPlan& right) noexcept {
    if (left.omissionCount != right.omissionCount) {
        return false;
    }
    for (std::uint32_t index = 0; index < left.omissionCount; ++index) {
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

/** Validates only identities required to mutate a connection-owned lease safely. */
[[nodiscard]] bool valid_mission_seed_plan(const ActivityMissionSeedPlan& plan,
                                           std::uint32_t scenarioRow) noexcept {
    const std::uint64_t authoredRegion =
        static_cast<std::uint64_t>(plan.sliceSetIndex) + plan.stateOrdinal;
    return plan.activityRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.scenarioRow == scenarioRow
           && plan.stateRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.bubbleRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.bubbleOrdinal < layouts::kBubbleCapacity
           && plan.stateOrdinal < tables::kSliceSetIndexFactor
           && authoredRegion == plan.effectiveRegion
           && authoredRegion
                  <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
}

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

/** @return True when a generated group owns the exact requested type-23 Auth slot. */
[[nodiscard]] bool valid_state_local_type23_target(const activity::host::ScriptableTarget& target,
                                                   const layouts::RosterGroup& group) noexcept {
    return target.stateLocalRoster && layouts::valid_roster_group(group)
           && target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
           && target.slotType == middleware::bap::activity_message::scriptable_auth::kType23SlotType
           && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType23Schema
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

/** @return True when a canonical group owns the exact requested type-23 Auth slot. */
[[nodiscard]] bool
valid_canonical_type23_target(const activity::host::ScriptableTarget& target) noexcept {
    if (target.stateLocalRoster || target.stateLocalRegion >= 0
        || target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
        || target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
        || target.slotType != middleware::bap::activity_message::scriptable_auth::kType23SlotType
        || target.authSchema != middleware::bap::activity_message::scriptable_auth::kType23Schema) {
        return false;
    }
    layouts::RosterGroup group{};
    return state::build_data::find_roster_group(target.rosterGroupIndex, group)
           && layouts::valid_roster_group(group) && group.objectTag == target.objectTag
           && group.registryKey == target.registryKey && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

/** @return True when a generated group owns the exact requested SDK Auth slot. */
[[nodiscard]] bool valid_state_local_sdk_auth_target(const activity::host::ScriptableTarget& target,
                                                     const layouts::RosterGroup& group) noexcept {
    return target.stateLocalRoster && layouts::valid_roster_group(group)
           && target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
           && target.slotType <= roster_message::kMaximumSlotType
           && target.slotIndex <= roster_message::kMaximumSlotIndex && target.authSchema != 0
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

/** @return True when a canonical roster group owns the exact requested SDK Auth slot. */
[[nodiscard]] bool
valid_canonical_sdk_auth_target(const activity::host::ScriptableTarget& target) noexcept {
    if (target.stateLocalRoster || target.stateLocalRegion >= 0
        || target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
        || target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
        || target.slotType > roster_message::kMaximumSlotType
        || target.slotIndex > roster_message::kMaximumSlotIndex || target.authSchema == 0) {
        return false;
    }
    layouts::RosterGroup group{};
    return state::build_data::find_roster_group(target.rosterGroupIndex, group)
           && layouts::valid_roster_group(group) && group.objectTag == target.objectTag
           && group.registryKey == target.registryKey && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

/** @return True when a generated group owns the exact requested type-31 Auth slot. */
[[nodiscard]] bool valid_state_local_type31_target(const activity::host::ScriptableTarget& target,
                                                   const layouts::RosterGroup& group) noexcept {
    return target.stateLocalRoster && layouts::valid_roster_group(group)
           && target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
           && target.slotType == middleware::bap::activity_message::scriptable_auth::kType31SlotType
           && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType31Schema
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
}

[[nodiscard]] bool session_scenario_layout(const Session& session,
                                           layouts::Definition& output) noexcept;

/** @return True when one canonical table is active; repeated publication of that table is one
 * target. */
[[nodiscard]] bool canonical_group_occurs_once(const layouts::Definition& layout,
                                               std::uint16_t tableIndex,
                                               std::int32_t region) noexcept {
    if (region < 0 || layout.rosterGroupCount == 0
        || layout.rosterGroupCount > layout.rosterGroups.size()
        || layout.bubbleGroupCount > layout.bubbleGroups.size()
        || layout.bubbleGroupCount > layout.bubbleGroupMasks.size()) {
        return false;
    }
    const std::uint32_t bubble = static_cast<std::uint32_t>(region) / tables::kSliceSetIndexFactor;
    if (bubble >= layouts::kBubbleCapacity) {
        return false;
    }
    std::size_t occurrences = 0;
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        occurrences += layout.rosterGroups[index] == tableIndex ? 1U : 0U;
    }
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        const bool active = (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << bubble)) != 0;
        occurrences += active && layout.bubbleGroups[index] == tableIndex ? 1U : 0U;
    }
    return occurrences != 0;
}

/** Checks one canonical type-23 request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool canonical_type23_available_locked(const Session& session,
                                                     const activity::host::ScriptableTarget& target,
                                                     std::int32_t expectedRegion,
                                                     std::uint64_t expectedGeneration) noexcept {
    const encrypted::push::activity::EffectiveRegion region = selected_region_locked(session);
    layouts::Definition layout{};
    return expectedRegion >= 0 && expectedGeneration != 0 && region.index == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_canonical_type23_target(target) && session_scenario_layout(session, layout)
           && canonical_group_occurs_once(layout, target.rosterGroupIndex, region.index);
}

/** Checks one lifetime request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool lifetime_available_locked(const Session& session,
                                             std::int32_t expectedRegion,
                                             std::uint64_t expectedGeneration) noexcept {
    const encrypted::push::activity::EffectiveRegion region = selected_region_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0 && region.index == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration;
}

/** Checks one generated type-23 request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
state_local_type23_available_locked(const Session& session,
                                    const activity::host::ScriptableTarget& target,
                                    const layouts::RosterGroup& stateLocalRosterGroup,
                                    std::int32_t expectedRegion,
                                    std::uint64_t expectedGeneration,
                                    std::uint32_t,
                                    std::uint32_t) noexcept {
    const encrypted::push::activity::EffectiveRegion region = selected_region_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0
           && session.activity.role == ActivityClientRole::privateCurrent
           && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_state_local_type23_target(target, stateLocalRosterGroup);
}

/** Checks one canonical SDK Auth request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
canonical_sdk_auth_available_locked(const Session& session,
                                    const activity::host::ScriptableTarget& target,
                                    std::int32_t expectedRegion,
                                    std::uint64_t expectedGeneration) noexcept {
    const encrypted::push::activity::EffectiveRegion region = selected_region_locked(session);
    layouts::Definition layout{};
    return expectedRegion >= 0 && expectedGeneration != 0 && region.index == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_canonical_sdk_auth_target(target) && session_scenario_layout(session, layout)
           && canonical_group_occurs_once(layout, target.rosterGroupIndex, region.index);
}

/** Checks one generated SDK Auth request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
state_local_sdk_auth_available_locked(const Session& session,
                                      const activity::host::ScriptableTarget& target,
                                      const layouts::RosterGroup& stateLocalRosterGroup,
                                      std::int32_t expectedRegion,
                                      std::uint64_t expectedGeneration,
                                      std::uint32_t,
                                      std::uint32_t) noexcept {
    const encrypted::push::activity::EffectiveRegion region = selected_region_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0
           && session.activity.role == ActivityClientRole::privateCurrent
           && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup);
}

/** @return True when a generated group owns the exact requested type-43 Auth slot. */
[[nodiscard]] bool
valid_state_local_authored_scene_target(const activity::host::ScriptableTarget& target,
                                        const layouts::RosterGroup& group) noexcept {
    return target.stateLocalRoster && layouts::valid_roster_group(group)
           && target.rosterGroupIndex == activity::host::kGeneratedRosterGroupIndex
           && target.sdkObjectIndex != activity::host::kNoSdkObjectIndex
           && target.slotType == roster_message::kAuthoredSceneSlotType
           && target.authSchema == roster_message::kAuthoredSceneAuthSchema
           && group.objectTag == target.objectTag && group.registryKey == target.registryKey
           && target.rosterSlotOffset < group.slotCount
           && group.slotTypes[target.rosterSlotOffset] == target.slotType
           && group.slotIndices[target.rosterSlotOffset] == target.slotIndex
           && (group.slotFlags[target.rosterSlotOffset] & roster_message::kSlotAuthFlag) != 0;
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

/** Loads the exact scenario layout owned by one lock-held ActivityClient. */
[[nodiscard]] bool session_scenario_layout(const Session& session,
                                           layouts::Definition& output) noexcept {
    output = {};
    const auto& selection = session.activity.session.destination;
    if (!state::activity::binding_matches(session.activity.session)
        || selection.packageNameLength > selection.packageName.size()) {
        return false;
    }
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    return state::build_data::find_scenario_layout(name, output);
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
    const std::int32_t selectedRegion = selected_region_locked(session).index;
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

/** @param session Its secrets and identity are wiped. */
void clear_session(Session& session) noexcept {
    SecureZeroMemory(&session, sizeof session);
    // Zeroing is not the cleared state: `advertisedRegion` is -1 and zero is a real region.
    session.activity = {};
}

/**
 * Releases an authenticated session's optional matchmaking context.
 * @param session Open session that may not have finished server hello.
 * @return True when there was no context, or the active generation was released.
 */
[[nodiscard]] bool release_matchmaking_context(Session& session) noexcept {
    if (session.matchmakingContext.generation == state::matchmaking::kInvalidGeneration) {
        session.matchmakingContext = {};
        return true;
    }
    if (!state::matchmaking::release_context(session.matchmakingContext)) {
        return false;
    }
    session.matchmakingContext = {};
    return true;
}

/** @param id Session-slot id. @return True when the slot is opened. */
[[nodiscard]] bool open_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    session.id = id;
    return true;
}

/** @param id Session-slot id. @return True when the slot is cleared. */
[[nodiscard]] bool close_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    return true;
}

/**
 * Routes one validated frame through its connection-owned session.
 * @param request Frame event and caller-owned buffers.
 * @param response Receives encoded response size.
 * @return True when the frame is valid and its service is handled.
 */
[[nodiscard]] bool consume_frame(const client::network::BapRequest& request,
                                 client::network::BapResponse& response) noexcept {
    middleware::bap::OuterFrame frame;
    if (!middleware::bap::parse_frame(request.frame, frame)) {
        return false;
    }
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    bool handled = false;
    if (frame.frameType == middleware::bap::FrameType::encrypted) {
        handled = encrypted::consume(*session, g_scratch, frame, request.response, response.size);
    } else {
        handled = plaintext::consume(*session, g_scratch, frame, request.response, response.size);
    }
    if (!handled) {
        return false;
    }
    if (frame.frameType == middleware::bap::FrameType::encrypted
        && session->accountMutationPublished) {
        publish_account_mutation(*session);
    }
    // A frame response can carry one already-due push in the same bounded socket write.
    bool touchesScratch = true;
    std::size_t deferred = 0;
    if (response.size < request.response.size()
        && encrypted::consume_deferred(*session,
                                       g_scratch,
                                       request.response.subspan(response.size),
                                       deferred,
                                       touchesScratch)) {
        response.size += deferred;
    }
    return true;
}

/**
 * Services one timed poll for a session that may owe a deferred push.
 * @param request Poll event and caller-owned output buffer.
 * @param response Receives encoded notification size.
 * @param touchesScratch Set when the attempt reaches a scratch buffer.
 * @return True when a notification is published.
 */
[[nodiscard]] bool consume_poll(const client::network::BapRequest& request,
                                client::network::BapResponse& response,
                                bool& touchesScratch) noexcept {
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    // The purchase response carries the Family-4 ownership rows. Refresh Family 5 only after the
    // client has consumed that response, so derived artifact state never mixes adjacent purchases.
    if (session->artifactRefreshArmed) {
        state::InvestmentState investment{};
        if (state::investment_snapshot(investment)
            && client::hooks::network::investment::publish_live_family5(investment.family5)) {
            session->artifactRefreshArmed = false;
        }
    }
    return encrypted::consume_deferred(
        *session, g_scratch, request.response, response.size, touchesScratch);
}

} // namespace

void arm_account_resync_elsewhere(Session& origin) noexcept {
    for (auto& peer : g_sessions) {
        if (&peer != &origin && peer.id != 0 && peer.authenticated && peer.queuez.family4Active) {
            peer.accountResyncArmed = true;
        }
    }
}

/** Arms every active peer to re-read the account, including the origin. */
void arm_account_resync_everywhere() noexcept {
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active) {
            continue;
        }
        peer.accountResyncArmed = true;
    }
}

void arm_acquisition_presentation_hold(Session& session) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (now >= session.acquisitionPresentationUntilTick) {
        session.acquisitionPresentationRows = {};
        session.acquisitionPresentationRowCount = 0;
    }
    session.acquisitionPresentationUntilTick =
        (std::max)(session.acquisitionPresentationUntilTick, now + kAcquisitionPresentationHoldMs);
}

bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept {
    return enqueue_world_reward({1, itemDefinitionIndex, WorldRewardKind::item});
}

bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                        std::int32_t quantity) noexcept {
    if (quantity <= 0) {
        return false;
    }
    return enqueue_world_reward({quantity, itemDefinitionIndex, WorldRewardKind::profileItem});
}

bool current_world_reward(WorldRewardRequest& request) noexcept {
    if (g_worldRewardCount == 0) {
        request = {};
        return false;
    }
    request = g_worldRewards[g_worldRewardHead];
    return true;
}

void complete_world_reward() noexcept {
    if (g_worldRewardCount == 0) {
        return;
    }
    pop_world_reward();
}

void fail_world_reward_attempt() noexcept {
    if (g_worldRewardCount == 0
        || ++g_worldRewards[g_worldRewardHead].failures < kWorldRewardFailureLimit) {
        return;
    }
    const bool committed = commit_world_reward(g_worldRewards[g_worldRewardHead]);
    pop_world_reward();
    if (committed) {
        arm_account_resync_everywhere();
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     committed ? "ev=world_reward stage=retry_limit result=direct"
                               : "ev=world_reward stage=retry_limit result=drop");
}

bool arm_seasonal_experience_presentation(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active
            || peer.pendingSeasonalExperienceAmount
                   > (std::numeric_limits<std::int32_t>::max)() - amount) {
            continue;
        }
        if (!state::progression::seasonal_experience::grant(amount)) {
            return false;
        }
        peer.pendingSeasonalExperienceAmount += amount;
        peer.pendingSeasonalExperienceFailures = 0;
        return true;
    }
    return false;
}

/** Applies one serialized BAP connection lifecycle event. */

/** Finds one unambiguous registry identity in a committed connection-local roster map. */
const RosterDecodeEntry* find_roster_decode_entry(const RosterDecodeMap& map,
                                                  std::uint64_t expectedBindingGeneration,
                                                  std::uint32_t registryKey) noexcept {
    if (!map.valid || expectedBindingGeneration == 0
        || map.bindingGeneration != expectedBindingGeneration || map.count > map.entries.size()) {
        return nullptr;
    }

    const RosterDecodeEntry* match = nullptr;
    for (std::size_t index = 0; index < map.count; ++index) {
        const RosterDecodeEntry& entry = map.entries[index];
        if (entry.registryKey != registryKey) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &entry;
    }
    return match;
}

/** Counts matching authenticated links while the caller already owns the BAP lock. */
std::size_t activity_link_count_locked(const state::activity::SessionBinding& binding) noexcept {
    std::size_t count = 0;
    static_cast<void>(unique_activity_link_locked(binding, count));
    return count;
}

/** Applies one serialized BAP connection lifecycle event. */
bool consume(const client::network::BapRequest& request,
             client::network::BapResponse& response) noexcept {
    response = {};
    const std::lock_guard lock(g_lock);
    bool success = false;
    // Polls report whether they reached scratch.
    bool touchesScratch = request.event != client::network::BapEvent::poll;
    // Hold the session lock across cryptographic counter reads and updates.
    switch (request.event) {
    case client::network::BapEvent::open:
        success = open_session(request.connectionId);
        break;
    case client::network::BapEvent::frame:
        success = consume_frame(request, response);
        break;
    case client::network::BapEvent::poll:
        success = consume_poll(request, response, touchesScratch);
        break;
    case client::network::BapEvent::close:
        success = close_session(request.connectionId);
        break;
    }
    // Decrypted frames can contain runtime-only keys or tokens, so scratch never outlives the call.
    if (touchesScratch) {
        SecureZeroMemory(&g_scratch, sizeof g_scratch);
    }
    return success;
}

/** Counts authenticated BAP links that currently own one exact activity generation. */
std::size_t activity_link_count(const state::activity::SessionBinding& binding) noexcept {
    const std::shared_lock lock(g_lock);
    const std::size_t count = activity_link_count_locked(binding);
    return count;
}

/** Reads the unique ActivityClient and the exact region its msg-5 builder will use. */
bool activity_link_view(const state::activity::SessionBinding& binding,
                        ActivityLinkView& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    const Session* const session = unique_activity_link_locked(binding, output.matchingLinks);
    if (session != nullptr) {
        const auto region = selected_region_locked(*session);
        output.activityClientGeneration = session->activity.bindingGeneration;
        output.effectiveRegion = region.index;
        output.sliceSetIndex =
            state::activity::membership::reported_slice_set(session->activity.session.sessionId);
        output.arrivalSliceSetIndex = static_cast<std::int32_t>(region.arrival);
        output.effectiveRegionReported = region.reported;
        output.joined = session->activity.role == ActivityClientRole::publicTarget
                        || session->activityJoinGeneration == session->activity.bindingGeneration;
        output.publicTarget = session->activity.role == ActivityClientRole::publicTarget;
        output.rosterReason = session->activityRosterReason;
        output.playerKey = encrypted::push::activity::published_player_key(*session);
    }
    return session != nullptr;
}

/** Checks whether one ActivityClient can accept state-local output for its published roster. */
ActivityMissionSeedLeaseStatus
activity_mission_seed_available(const state::activity::SessionBinding& binding,
                                std::uint32_t scenarioRow,
                                std::uint64_t expectedGeneration) noexcept {
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    std::size_t matchingLinks = 0;
    ActivityMissionSeedLeaseStatus status =
        mission_seed_link_locked(binding, scenarioRow, expectedGeneration, session, matchingLinks);
    if (status == ActivityMissionSeedLeaseStatus::ready && session->activityRosterStaged.staged) {
        status = ActivityMissionSeedLeaseStatus::outputBusy;
    }
    return status;
}

/** Reads one exact ActivityClient's connection-scoped SDK selected-state roster lease. */
ActivityMissionSeedLeaseStatus
activity_mission_seed_lease(const state::activity::SessionBinding& binding,
                            std::uint32_t scenarioRow,
                            std::uint64_t expectedGeneration,
                            ActivityMissionSeedLeaseView& output) noexcept {
    output = {};
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    ActivityMissionSeedLeaseStatus status = mission_seed_link_locked(
        binding, scenarioRow, expectedGeneration, session, output.matchingLinks);
    if (session != nullptr) {
        output.activityClientGeneration = session->activity.bindingGeneration;
    }
    if (status == ActivityMissionSeedLeaseStatus::ready) {
        read_mission_seed_lease(*session, output.matchingLinks, output);
    }
    return status;
}

/** Selects one exact materialized state and advances its publication revision once. */
ActivityMissionSeedLeaseStatus
select_activity_mission_seed(const state::activity::SessionBinding& binding,
                             const ActivityMissionSeedPlan& plan,
                             std::uint64_t expectedGeneration) noexcept {
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    std::size_t matchingLinks = 0;
    ActivityMissionSeedLeaseStatus status = mission_seed_link_locked(
        binding, plan.scenarioRow, expectedGeneration, session, matchingLinks);
    if (status == ActivityMissionSeedLeaseStatus::ready
        && !valid_mission_seed_plan(plan, plan.scenarioRow)) {
        status = ActivityMissionSeedLeaseStatus::refused;
    }
    if (status == ActivityMissionSeedLeaseStatus::ready) {
        MissionSeedLease& lease = session->activityMissionSeed;
        if (lease.configured && same_mission_seed_plan(lease.plan, plan)) {
            // The script may select the plan the roster adopted by default. That is a selection.
            lease.scriptSelected = true;
            return ActivityMissionSeedLeaseStatus::ready;
        }
        if (lease.configured && lease.revision == (std::numeric_limits<std::uint64_t>::max)()) {
            status = ActivityMissionSeedLeaseStatus::refused;
        } else {
            // A staged older revision may finish. Its commit cannot publish this newer revision.
            const std::uint64_t revision = lease.configured ? lease.revision + 1U : 1U;
            // Every region this lease has selected stays registered on the peer, so record the
            // new one and keep the earlier ones. Publication carries the union; dropping a group
            // does not unregister it, it only stops seeding records the peer still holds.
            if (!lease.configured) {
                lease.registeredRegionCount = 0;
            }
            bool regionKnown = false;
            for (std::size_t index = 0; index < lease.registeredRegionCount; ++index) {
                regionKnown = regionKnown || lease.registeredRegions[index] == plan.effectiveRegion;
            }
            if (!regionKnown) {
                if (lease.registeredRegionCount >= lease.registeredRegions.size()) {
                    return ActivityMissionSeedLeaseStatus::refused;
                }
                lease.registeredRegions[lease.registeredRegionCount++] = plan.effectiveRegion;
            }
            // A region change replaces the instantiated world. Publications keep answering the
            // previous plan until the client's post-arrival solicited answer advances the region
            // epoch, because registering the new region's groups mid-teardown races the teardown.
            if (lease.configured && lease.plan.effectiveRegion != plan.effectiveRegion) {
                lease.previousPlan = lease.plan;
                lease.regionArrivalPending = true;
            }
            lease.plan = plan;
            lease.bindingGeneration = session->activity.bindingGeneration;
            lease.revision = revision;
            lease.configured = true;
            lease.scriptSelected = true;
        }
    }
    return status;
}

/** Read-only check of a canonical type-23 target on one live ActivityClient generation. */
bool activity_type23_override_available(const state::activity::SessionBinding& binding,
                                        const activity::host::ScriptableTarget& target,
                                        std::int32_t expectedRegion,
                                        std::uint64_t expectedGeneration) noexcept {
    const std::shared_lock lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool available =
        session != nullptr
        && canonical_type23_available_locked(*session, target, expectedRegion, expectedGeneration);
    return available;
}

/** Selects the exact live ActivityClient for the client's local world slice. */
bool current_activity_link_view(std::int32_t localSliceSet,
                                CurrentActivityLinkView& output) noexcept {
    output = {};
    const Session* only = nullptr;
    const Session* matched = nullptr;
    const Session* coherent = nullptr;
    const Session* privateCurrent = nullptr;
    bool oneDestination = true;
    const std::shared_lock lock(g_lock);
    for (const Session& session : g_sessions) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || !state::activity::binding_matches(session.activity.session)) {
            continue;
        }
        only = &session;
        ++output.activeLinks;
        if (session.activity.role == ActivityClientRole::privateCurrent
            && (privateCurrent == nullptr
                || session.activity.bindingGeneration
                       > privateCurrent->activity.bindingGeneration)) {
            privateCurrent = &session;
        }
        if (coherent == nullptr) {
            coherent = &session;
        } else {
            oneDestination =
                oneDestination
                && same_destination_package(coherent->activity.session.destination,
                                            session.activity.session.destination);
            // Prefer the persistent private-current link for destination metadata. Public targets
            // are disposable region views and can overlap while the client changes bubbles.
            if ((session.activity.role == ActivityClientRole::privateCurrent
                 && coherent->activity.role != ActivityClientRole::privateCurrent)
                || (session.activity.role == coherent->activity.role
                    && session.activity.bindingGeneration
                           > coherent->activity.bindingGeneration)) {
                coherent = &session;
            }
        }
        if (localSliceSet >= 0 && selected_region_locked(session).index == localSliceSet) {
            ++output.matchingRegions;
            // A public region may be represented by several overlapping links. Prefer the stable
            // private-current owner; within one role, use the newest binding.
            const bool moreSpecific =
                matched == nullptr
                || (session.activity.role == ActivityClientRole::privateCurrent
                    && matched->activity.role != ActivityClientRole::privateCurrent)
                || (session.activity.role == matched->activity.role
                    && session.activity.bindingGeneration
                           > matched->activity.bindingGeneration);
            if (moreSpecific) {
                matched = &session;
            }
        }
    }
    const Session* const selected = privateCurrent != nullptr ? privateCurrent
                                    : matched != nullptr       ? matched
                                    : output.activeLinks == 1 ? only
                                    : oneDestination          ? coherent
                                                              : nullptr;
    if (selected != nullptr) {
        output.binding = selected->activity.session;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.effectiveRegion = selected_region_locked(*selected).index;
        output.publicTarget = selected->activity.role == ActivityClientRole::publicTarget;
    }
    return selected != nullptr;
}

/** Resolves world actions through the current player's actual region owner. */
bool current_activity_host_link_view(std::int32_t localSliceSet,
                                     CurrentActivityLinkView& output) noexcept {
    output = {};
    if (localSliceSet < 0) return false;
    const std::shared_lock lock(g_lock);
    std::array<host_selection::Candidate, kSessionCount> candidates{};
    std::array<const Session*, kSessionCount> sessions{};
    std::size_t count = 0;
    for (const Session& session : g_sessions) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || !state::activity::binding_matches(session.activity.session)
            || !state::activity::binding_matches(session.activity.source)) continue;
        const auto& link = session.activity;
        candidates[count] = {link.session.sessionId, link.session.createdRevision,
                             link.source.sessionId, link.source.createdRevision,
                             link.bindingGeneration, selected_region_locked(session).index,
                             link.role == ActivityClientRole::publicTarget};
        sessions[count++] = &session;
    }
    output.activeLinks = count;
    const std::span<const host_selection::Candidate> rows(candidates.data(), count);
    const auto source = host_selection::current_private(rows);
    if (source == host_selection::absent) return false;
    bool isPublic = false;
    std::optional<bool> privateRegion;
    if (encrypted::push::activity::region_publicity(*sessions[source], localSliceSet, isPublic)) {
        privateRegion = !isPublic;
    }
    const auto selected = host_selection::region_host(rows, source, localSliceSet, privateRegion);
    if (selected == host_selection::absent) return false;
    output.binding = sessions[selected]->activity.session;
    output.activityClientGeneration = rows[selected].generation;
    output.effectiveRegion = localSliceSet;
    output.publicTarget = rows[selected].publicTarget;
    output.matchingRegions = 1;
    return true;
}

/** Reads one exact ActivityClient's common-root inputs. */
bool activity_replication_view(const state::activity::SessionBinding& binding,
                               ActivityReplicationView& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    std::size_t count = 0;
    const Session* const session = unique_activity_link_locked(binding, count);
    const bool ready =
        session != nullptr && session->activityPatchEpoch.seen
        && session->activityPatchEpoch.bindingGeneration == session->activity.bindingGeneration;
    if (ready) {
        output.binding = session->activity.session;
        output.patchEpoch = session->activityPatchEpoch.value;
        output.activityClientGeneration = session->activity.bindingGeneration;
        output.groupSessionId = session->activity.groupSessionId;
        output.memberId = session->activityMemberKey;
        output.replicationEpoch = session->activity.replicationEpoch;
    }
    return ready;
}

/** Reads the unique ActivityClient whose activity session is the supplied view token. */
bool activity_replication_view_for_session(std::uint64_t activitySessionId,
                                           ActivityReplicationView& output) noexcept {
    output = {};
    if (activitySessionId == 0) {
        return false;
    }
    const std::shared_lock lock(g_lock);
    const Session* selected = nullptr;
    std::size_t count = 0;
    for (const Session& session : g_sessions) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || session.activity.session.sessionId != activitySessionId
            || !state::activity::binding_matches(session.activity.session)
            || !session.activityPatchEpoch.seen
            || session.activityPatchEpoch.bindingGeneration != session.activity.bindingGeneration) {
            continue;
        }
        selected = &session;
        ++count;
    }
    if (count == 1 && selected != nullptr) {
        output.binding = selected->activity.session;
        output.patchEpoch = selected->activityPatchEpoch.value;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.groupSessionId = selected->activity.groupSessionId;
        output.memberId = selected->activityMemberKey;
        output.replicationEpoch = selected->activity.replicationEpoch;
    }
    return count == 1;
}

/** Reads the unique ActivityClient bound to one gameplay group session. */
bool activity_replication_view_for_group(std::uint64_t groupSessionId,
                                         ActivityReplicationView& output) noexcept {
    output = {};
    if (groupSessionId == 0) {
        return false;
    }
    const std::shared_lock lock(g_lock);
    const Session* selected = nullptr;
    std::size_t count = 0;
    for (const Session& session : g_sessions) {
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0
            || session.activity.groupSessionId != groupSessionId || !session.activityPatchEpoch.seen
            || session.activityPatchEpoch.bindingGeneration != session.activity.bindingGeneration) {
            continue;
        }
        selected = &session;
        ++count;
    }
    if (count == 1 && selected != nullptr) {
        output.binding = selected->activity.session;
        output.patchEpoch = selected->activityPatchEpoch.value;
        output.activityClientGeneration = selected->activity.bindingGeneration;
        output.groupSessionId = selected->activity.groupSessionId;
        output.memberId = selected->activityMemberKey;
        output.replicationEpoch = selected->activity.replicationEpoch;
    }
    return count == 1;
}

/** Queues one generation-bound replication epoch request. */
bool request_replication_epoch(const state::activity::SessionBinding& binding,
                               std::uint64_t expectedGeneration,
                               std::uint8_t generation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t count = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, count);
    bool queued = session != nullptr && expectedGeneration != 0
                  && session->activity.bindingGeneration == expectedGeneration;
    if (queued) {
        ReplicationEpochPublication& request = session->activityReplicationEpoch;
        queued = !request.pending
                 || (request.bindingGeneration == expectedGeneration
                     && request.generation == generation);
        if (queued) {
            request.bindingGeneration = expectedGeneration;
            request.generation = generation;
            request.pending = true;
            request.staged = false;
            session->activityKeepaliveDueTick = 0;
        }
    }
    return queued;
}

/** Queues one msg-30 readback on an exact unique ActivityClient link. */
ActivityAuthorityQueryStatus
request_activity_authority_query(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept {
    correlation = -1;
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, linkCount);
    ActivityAuthorityQueryStatus status = ActivityAuthorityQueryStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_query::request(
                           session->activityAuthorityQuery, expectedGeneration, correlation)
                     : ActivityAuthorityQueryStatus::staleActivityClient;
    }
    return status;
}

/** Copies one complete connection-owned authority readback. */
ActivityAuthorityQueryStatus
activity_authority_query_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityQuerySnapshot& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    ActivityAuthorityQueryStatus status = ActivityAuthorityQueryStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_query::snapshot(
                           session->activityAuthorityQuery, expectedGeneration, output)
                     : ActivityAuthorityQueryStatus::staleActivityClient;
    }
    return status;
}

/** Queues one msg-28 rebuild on an exact unique ActivityClient link. */
ActivityAuthorityResetStatus
request_activity_authority_reset(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept {
    correlation = -1;
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, linkCount);
    ActivityAuthorityResetStatus status = ActivityAuthorityResetStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_reset::request(
                           session->activityAuthorityReset, expectedGeneration, correlation)
                     : ActivityAuthorityResetStatus::staleActivityClient;
    }
    return status;
}

/** Copies one complete connection-owned authority-reset result. */
ActivityAuthorityResetStatus
activity_authority_reset_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityResetSnapshot& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    ActivityAuthorityResetStatus status = ActivityAuthorityResetStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_reset::snapshot(
                           session->activityAuthorityReset, expectedGeneration, output)
                     : ActivityAuthorityResetStatus::staleActivityClient;
    }
    return status;
}

/** Queues a type-23 override only while exactly one authenticated link owns the binding. */
bool request_activity_type23_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
    float value,
    bool snap,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued =
        session != nullptr
        && canonical_type23_available_locked(*session, target, expectedRegion, expectedGeneration)
        && activity::host::request_type23_override(
            binding, target, channel, value, snap, expectedGeneration, reservation);
    return queued;
}

/** Queues one activity lifetime change while exactly one authenticated link owns the binding. */
bool request_activity_lifetime_override(
    const state::activity::SessionBinding& binding,
    std::uint8_t lifetimeState,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued = session != nullptr
                        && lifetime_available_locked(*session, expectedRegion, expectedGeneration)
                        && activity::host::request_lifetime_override(
                            binding, lifetimeState, expectedGeneration, reservation);
    return queued;
}

/** Queues a generated type-23 update while its exact state is live. */
bool request_activity_state_local_type23_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const layouts::RosterGroup& stateLocalRosterGroup,
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
    float value,
    bool snap,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued =
        session != nullptr
        && state_local_type23_available_locked(*session,
                                               target,
                                               stateLocalRosterGroup,
                                               expectedRegion,
                                               expectedGeneration,
                                               scenarioRow,
                                               stateRow)
        && activity::host::request_state_local_type23_override(binding,
                                                               target,
                                                               stateLocalRosterGroup,
                                                               channel,
                                                               value,
                                                               snap,
                                                               expectedGeneration,
                                                               reservation);
    return queued;
}

/** Queues one structurally compiled SDK Auth body on an exact live target. */
bool request_activity_sdk_auth_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const layouts::RosterGroup* stateLocalRosterGroup,
    std::span<const std::byte> body,
    std::uint16_t bitCount,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool available =
        session != nullptr
        && (target.stateLocalRoster
                ? stateLocalRosterGroup != nullptr
                      && state_local_sdk_auth_available_locked(*session,
                                                               target,
                                                               *stateLocalRosterGroup,
                                                               expectedRegion,
                                                               expectedGeneration,
                                                               scenarioRow,
                                                               stateRow)
                : stateLocalRosterGroup == nullptr
                      && canonical_sdk_auth_available_locked(
                          *session, target, expectedRegion, expectedGeneration));
    const bool queued = available
                        && activity::host::request_sdk_auth_override(binding,
                                                                     target,
                                                                     stateLocalRosterGroup,
                                                                     body,
                                                                     bitCount,
                                                                     expectedGeneration,
                                                                     reservation);
    return queued;
}

/** Queues a type-31 pulse only while exactly one authenticated link owns the binding. */
bool request_activity_type31_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    std::int32_t expectedRegion,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued = expectedRegion >= 0 && session != nullptr
                        && selected_region_locked(*session).index == expectedRegion
                        && activity::host::request_type31_override(binding, target, reservation);
    return queued;
}

/** Queues one generation-bound type-31 pulse for the selected live state. */
bool request_activity_state_local_type31_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_type31_target(target, stateLocalRosterGroup)
        && activity::host::request_state_local_type31_override(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Queues one sequence restart from an exact published SDK mission seed. */
bool request_activity_state_local_sequence_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType5SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType5Schema
        && activity::host::request_state_local_sequence_override(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Queues one cinematic transition from an exact published SDK mission seed. */
bool request_activity_state_local_cinematic_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    bool active,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType6SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType6Schema
        && activity::host::request_state_local_cinematic_override(
            binding, target, stateLocalRosterGroup, active, expectedGeneration, reservation);
    return queued;
}

/** Queues one performance start from an exact published SDK mission seed. */
bool request_activity_state_local_performance_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t stateNameHash,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType42SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType42Schema
        && activity::host::request_state_local_performance_override(
            binding, target, stateLocalRosterGroup, stateNameHash, expectedGeneration, reservation);
    return queued;
}

/** Queues one authored-scene activation from an exact published SDK mission seed. */
bool request_activity_state_local_authored_scene_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_authored_scene_target(target, stateLocalRosterGroup)
        && activity::host::request_state_local_authored_scene_override(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Queues one bounded authored-dialogue line from an exact published SDK mission seed. */
bool request_activity_state_local_dialogue_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint16_t cueIndex,
    std::uint16_t authoredCueCount,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType53SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType53Schema
        && activity::host::request_state_local_dialogue_override(binding,
                                                                 target,
                                                                 stateLocalRosterGroup,
                                                                 cueIndex,
                                                                 authoredCueCount,
                                                                 expectedGeneration,
                                                                 reservation);
    return queued;
}

/** Queues one objective reset from an exact published SDK mission seed. */
bool request_activity_state_local_objective_reset(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType3SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType3Schema
        && activity::host::request_state_local_objective_reset(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Queues one authored-task generation from an exact published SDK mission seed. */
bool request_activity_state_local_task_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t,
    std::uint32_t,
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent
        && region.index == expectedRegion && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType38SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType38Schema
        && activity::host::request_state_local_task_override(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Queues a squad placement only while exactly one authenticated link owns the binding. */
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
    std::array<std::int8_t, 4> authoredProfile) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const encrypted::push::activity::EffectiveRegion region =
        session != nullptr ? selected_region_locked(*session)
                           : encrypted::push::activity::EffectiveRegion{};
    const bool queued = expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
                        && session->activity.bindingGeneration == expectedGeneration
                        && squad_override_available_locked(
                            *session, target, stateLocalRosterGroup, expectedGeneration)
                        && region.index == expectedRegion
                        && activity::host::request_squad_override(binding,
                                                                  target,
                                                                  stateLocalRosterGroup,
                                                                  requestedCounts,
                                                                  mode,
                                                                  expectedGeneration,
                                                                  nameHash,
                                                                  reservation,
                                                                  authoredProfile);
    return queued;
}

/** Cancels one exact typed override revision while excluding activity-link publication. */
bool cancel_activity_scriptable_override(const state::activity::SessionBinding& binding,
                                         std::uint64_t expectedRevision) noexcept {
    const std::lock_guard lock(g_lock);
    const bool canceled =
        activity::host::cancel_pending_scriptable_override(binding, expectedRevision);
    return canceled;
}

/** Cancels a pending raw incident while excluding activity-link publication. */
bool cancel_activity_host_incident(const state::activity::SessionBinding& binding) noexcept {
    const std::lock_guard lock(g_lock);
    const bool canceled = server::activity::host::cancel_pending_incident(binding);
    return canceled;
}

#if defined(SUNRISE_BAP_FRAME_TEST)
/** Copies one armed connection's own send nonce and session key. Test-only, never shipped. */
bool session_channel(std::uint32_t connectionId,
                     std::array<std::byte, state::kBapNonceSize>& sendNonce,
                     std::array<std::byte, state::kAesKeySize>& sessionKey) noexcept {
    const std::shared_lock lock(g_lock);
    const Session* const session = session_for(connectionId);
    const bool armed = session != nullptr && session->authenticated;
    if (armed) {
        sendNonce = session->sendNonce;
        sessionKey = session->sessionKey;
    }
    return armed;
}
#endif

/** Securely erases every connection-owned nonce and transform buffer. */
void shutdown() noexcept {
    const std::lock_guard lock(g_lock);
    while (g_worldRewardCount != 0) {
        if (!commit_world_reward(g_worldRewards[g_worldRewardHead])) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=world_reward stage=shutdown result=drop");
        }
        pop_world_reward();
    }
    for (auto& session : g_sessions) {
        if (session.id != 0
            && session.matchmakingContext.generation != state::matchmaking::kInvalidGeneration) {
            // State erases runtime descriptors before the opaque association is cleared.
            (void)state::matchmaking::release_context(session.matchmakingContext);
        }
        if (session.id != 0) {
            encrypted::release_activity_connection(session);
        }
    }
    SecureZeroMemory(g_sessions.data(), sizeof g_sessions);
    g_worldRewards = {};
    g_worldRewardHead = 0;
    g_worldRewardCount = 0;
    SecureZeroMemory(&g_scratch, sizeof g_scratch);
}

} // namespace sunrise::server::bap
