/** Scriptable Auth overrides: target eligibility and the entry points a mission script calls. */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>

#include "../../state/build_data/runtime.h"
#include "../activity/host_runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {

namespace layouts = state::build_data::scenarios;
namespace roster_message = middleware::bap::activity_message::sensor_auth_update;
namespace tables = middleware::content::packages::tables;

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
    const std::int32_t region = selected_region_index_locked(session);
    layouts::Definition layout{};
    return expectedRegion >= 0 && expectedGeneration != 0 && region == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_canonical_type23_target(target) && session_scenario_layout(session, layout)
           && canonical_group_occurs_once(layout, target.rosterGroupIndex, region);
}

/** Checks one lifetime request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool lifetime_available_locked(const Session& session,
                                             std::int32_t expectedRegion,
                                             std::uint64_t expectedGeneration) noexcept {
    const std::int32_t region = selected_region_index_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0 && region == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration;
}

/** Checks one generated type-23 request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
state_local_type23_available_locked(const Session& session,
                                    const activity::host::ScriptableTarget& target,
                                    const layouts::RosterGroup& stateLocalRosterGroup,
                                    std::int32_t expectedRegion,
                                    std::uint64_t expectedGeneration) noexcept {
    const std::int32_t region = selected_region_index_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0
           && session.activity.role == ActivityClientRole::privateCurrent
           && region == expectedRegion && target.stateLocalRegion == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_state_local_type23_target(target, stateLocalRosterGroup);
}

/** Checks one canonical SDK Auth request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
canonical_sdk_auth_available_locked(const Session& session,
                                    const activity::host::ScriptableTarget& target,
                                    std::int32_t expectedRegion,
                                    std::uint64_t expectedGeneration) noexcept {
    const std::int32_t region = selected_region_index_locked(session);
    layouts::Definition layout{};
    return expectedRegion >= 0 && expectedGeneration != 0 && region == expectedRegion
           && session.activity.bindingGeneration == expectedGeneration
           && valid_canonical_sdk_auth_target(target) && session_scenario_layout(session, layout)
           && canonical_group_occurs_once(layout, target.rosterGroupIndex, region);
}

/** Checks one generated SDK Auth request while the BAP lock owns ActivityClient state. */
[[nodiscard]] bool
state_local_sdk_auth_available_locked(const Session& session,
                                      const activity::host::ScriptableTarget& target,
                                      const layouts::RosterGroup& stateLocalRosterGroup,
                                      std::int32_t expectedRegion,
                                      std::uint64_t expectedGeneration) noexcept {
    const std::int32_t region = selected_region_index_locked(session);
    return expectedRegion >= 0 && expectedGeneration != 0
           && session.activity.role == ActivityClientRole::privateCurrent
           && region == expectedRegion && target.stateLocalRegion == expectedRegion
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

} // namespace

/** Read-only check of a canonical type-23 target on one live ActivityClient generation. */
bool activity_type23_override_available(const state::activity::SessionBinding& binding,
                                        const activity::host::ScriptableTarget& target,
                                        std::int32_t expectedRegion,
                                        std::uint64_t expectedGeneration) noexcept {
    const std::shared_lock lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool available =
        session != nullptr
        && canonical_type23_available_locked(*session, target, expectedRegion, expectedGeneration);
    return available;
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
    const std::lock_guard lock(session_lock());
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
    const std::lock_guard lock(session_lock());
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued =
        session != nullptr
        && state_local_type23_available_locked(
            *session, target, stateLocalRosterGroup, expectedRegion, expectedGeneration)
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
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
                                                               expectedGeneration)
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
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const bool queued = expectedRegion >= 0 && session != nullptr
                        && selected_region_index_locked(*session) == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation,
    const activity::host::AuthoredSceneDependencies& dependencies,
    std::uint32_t eventKey,
    bool stop) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_authored_scene_target(target, stateLocalRosterGroup)
        && activity::host::request_state_local_authored_scene_override(binding,
                                                                       target,
                                                                       stateLocalRosterGroup,
                                                                       expectedGeneration,
                                                                       reservation,
                                                                       dependencies,
                                                                       eventKey,
                                                                       stop);
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
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
    const activity::host::ScriptableOutputReservation* reservation) noexcept {
    const std::lock_guard lock(session_lock());
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    const std::int32_t region = session != nullptr ? selected_region_index_locked(*session) : -1;
    const bool queued =
        expectedRegion >= 0 && expectedGeneration != 0 && session != nullptr
        && session->activity.role == ActivityClientRole::privateCurrent && region == expectedRegion
        && target.stateLocalRegion == expectedRegion
        && session->activity.bindingGeneration == expectedGeneration
        && valid_state_local_sdk_auth_target(target, stateLocalRosterGroup)
        && target.slotType == middleware::bap::activity_message::scriptable_auth::kType38SlotType
        && target.authSchema == middleware::bap::activity_message::scriptable_auth::kType38Schema
        && activity::host::request_state_local_task_override(
            binding, target, stateLocalRosterGroup, expectedGeneration, reservation);
    return queued;
}

/** Cancels one exact typed override revision while excluding activity-link publication. */
bool cancel_activity_scriptable_override(const state::activity::SessionBinding& binding,
                                         std::uint64_t expectedRevision) noexcept {
    const std::lock_guard lock(session_lock());
    const bool canceled =
        activity::host::cancel_pending_scriptable_override(binding, expectedRevision);
    return canceled;
}

} // namespace sunrise::server::bap
