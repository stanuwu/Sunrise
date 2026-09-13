#include <algorithm>

#include "../../core/logging/log.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::state::activity_sdk {
namespace {

/** @return True when two generated groups carry the same complete msg-5 layout. */
[[nodiscard]] bool
same_roster_group(const state::build_data::scenarios::RosterGroup& left,
                  const state::build_data::scenarios::RosterGroup& right) noexcept {
    return left.registryKey == right.registryKey && left.objectTag == right.objectTag
           && left.slotCount == right.slotCount && left.slotTypes == right.slotTypes
           && left.slotFlags == right.slotFlags && left.slotIndices == right.slotIndices;
}

/** Withdraws any groups written before a fail-closed mission-plan result. */
MissionSeedStatus refuse_mission_seed(MissionSeedStatus status,
                                      std::span<state::build_data::scenarios::RosterGroup> groups,
                                      std::size_t written,
                                      MissionSeedSummary& output) noexcept {
    const std::size_t count = (std::min)(written, groups.size());
    for (std::size_t index = 0; index < count; ++index) {
        groups[index] = {};
    }
    output = {};
    return status;
}

} // namespace

/** Builds the wire roster shape from one validated generated object definition. */
bool materialize_roster_group(const Catalog& catalog,
                              const format::Object& object,
                              state::build_data::scenarios::RosterGroup& output) noexcept {
    namespace layouts = state::build_data::scenarios;
    output = {};
    const auto slots = object_slots(catalog, object);
    if (slots.empty() || object.objectTag == 0 || object.objectKey == 0) {
        return false;
    }
    output.objectTag = object.objectTag;
    output.registryKey = object.objectKey;
    for (const format::Slot& slot : slots) {
        const bool descriptorBacked = slot.componentClass != format::kAbsentIndex;
        if (!descriptorBacked) {
            if (slot.senseSchema != format::kAbsentIndex || slot.authSchema != format::kAbsentIndex
                || slot.flags != 0) {
                output = {};
                return false;
            }
            continue;
        }
        if (slot.slotIndex >= layouts::kRosterSlotCapacity || slot.slotType == 0
            || slot.slotType > layouts::kMaximumSlotType
            || (output.slotCount != 0 && slot.slotIndex <= output.slotIndices[output.slotCount - 1])
            || output.slotCount >= output.slotIndices.size()) {
            output = {};
            return false;
        }
        const std::size_t index = output.slotCount;
        output.slotIndices[index] = static_cast<std::uint16_t>(slot.slotIndex);
        output.slotTypes[index] = static_cast<std::uint8_t>(slot.slotType);
        if (slot.senseSchema != format::kAbsentIndex) {
            output.slotFlags[index] |= layouts::kSlotSenseFlag;
        }
        if (slot.authSchema != format::kAbsentIndex) {
            output.slotFlags[index] |= layouts::kSlotAuthFlag;
        }
        ++output.slotCount;
    }
    return layouts::valid_roster_group(output);
}

/** Reports whether one generated group occurs in every enabled state of its scenario. */
bool mission_seed_group_is_scenario_wide(const BoundView& view,
                                         std::uint32_t objectTag,
                                         std::uint32_t registryKey,
                                         bool& scenarioWide) noexcept {
    scenarioWide = false;
    if (view.catalog == nullptr || objectTag == 0 || registryKey == 0) {
        return false;
    }
    const Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = bound_scenario(view);
    if (scenario == nullptr) {
        return false;
    }
    const auto objects = catalog.objects();
    const format::Object* selectedObject = nullptr;
    for (const format::Object& object : objects) {
        if (object.objectTag != objectTag || object.objectKey != registryKey) {
            continue;
        }
        if (selectedObject != nullptr) {
            return false;
        }
        selectedObject = &object;
    }
    if (selectedObject == nullptr) {
        return false;
    }
    const std::uint32_t objectIndex = static_cast<std::uint32_t>(selectedObject - objects.data());
    const auto allStates = catalog.states();
    const auto occurrences = scenario_occurrences(catalog, *scenario);
    std::size_t enabledStates = 0;
    for (const format::State& state : scenario_states(catalog, *scenario)) {
        if ((state.flags & format::kStateFlagMask) != format::kStateFlagMask) {
            continue;
        }
        ++enabledStates;
        const std::uint32_t stateIndex = static_cast<std::uint32_t>(&state - allStates.data());
        bool present = false;
        for (const format::Occurrence& occurrence : occurrences) {
            if (occurrence.stateIndex == stateIndex && occurrence.objectIndex == objectIndex) {
                if (present) {
                    return false;
                }
                present = true;
            }
        }
        if (!present) {
            scenarioWide = false;
            return true;
        }
    }
    if (enabledStates == 0) {
        return false;
    }
    scenarioWide = true;
    return true;
}

/** Builds the exact selected-state roster seed for one bound scenario and package region. */
MissionSeedStatus
materialize_initial_mission_seed(const BoundView& view,
                                 std::int32_t requestedEffectiveRegion,
                                 std::span<const MissionSeedOmission> omissions,
                                 std::span<state::build_data::scenarios::RosterGroup> outputGroups,
                                 MissionSeedSummary& output) noexcept {
    namespace layouts = state::build_data::scenarios;
    output = {};
    if (omissions.size() > kMissionSeedOmitCapacity) {
        return MissionSeedStatus::invalidView;
    }
    if (requestedEffectiveRegion < 0 || view.catalog == nullptr) {
        return requestedEffectiveRegion < 0 ? MissionSeedStatus::invalidSliceSet
                                            : MissionSeedStatus::invalidView;
    }
    const Catalog& catalog = *view.catalog;
    const format::Activity* const activity = bound_activity(view);
    const format::Scenario* const scenario = bound_scenario(view);
    if (activity == nullptr || scenario == nullptr || activity->scenarioIndex != view.scenarioRow
        || (activity->flags & format::kActivityExactMask) != format::kActivityExactMask) {
        return MissionSeedStatus::invalidView;
    }

    const std::uint32_t requestedRegion = static_cast<std::uint32_t>(requestedEffectiveRegion);
    const format::State* selectedState = nullptr;
    std::uint32_t selectedStateRow = format::kAbsentIndex;
    const auto allStates = catalog.states();
    for (const format::State& state : scenario_states(catalog, *scenario)) {
        const std::uint64_t authoredRegion =
            static_cast<std::uint64_t>(state.sliceSetIndex) + state.stateOrdinal;
        if ((state.flags & format::kStateFlagMask) != format::kStateFlagMask
            || authoredRegion != requestedRegion) {
            continue;
        }
        if (selectedState != nullptr) {
            return MissionSeedStatus::ambiguousInitialState;
        }
        selectedState = &state;
        selectedStateRow = static_cast<std::uint32_t>(&state - allStates.data());
    }
    if (selectedState == nullptr) {
        return MissionSeedStatus::missingInitialState;
    }
    if (selectedState->bubbleIndex >= catalog.bubbles().size()) {
        return MissionSeedStatus::invalidView;
    }
    const format::Bubble& bubble = catalog.bubbles()[selectedState->bubbleIndex];
    if (bubble.scenarioIndex != view.scenarioRow) {
        return MissionSeedStatus::invalidView;
    }

    MissionSeedSummary candidate{};
    candidate.activityRow = view.activityRow;
    candidate.scenarioRow = view.scenarioRow;
    candidate.stateRow = selectedStateRow;
    candidate.bubbleRow = selectedState->bubbleIndex;
    candidate.bubbleOrdinal = bubble.bubbleOrdinal;
    candidate.stateOrdinal = selectedState->stateOrdinal;
    candidate.entryIndex = selectedState->entryIndex;
    candidate.sliceSetIndex = selectedState->sliceSetIndex;
    candidate.effectiveRegion = requestedRegion;
    for (std::size_t index = 0; index < omissions.size(); ++index) {
        candidate.omissions[index] = omissions[index];
    }
    candidate.omissionCount = static_cast<std::uint32_t>(omissions.size());

    std::size_t groupCount = 0;
    for (const format::Occurrence& occurrence : scenario_occurrences(catalog, *scenario)) {
        if (occurrence.stateIndex != selectedStateRow) {
            continue;
        }
        ++candidate.occurrenceCount;
        if (occurrence.scenarioIndex != view.scenarioRow
            || occurrence.bubbleIndex != selectedState->bubbleIndex
            || occurrence.objectIndex >= catalog.objects().size()) {
            return refuse_mission_seed(
                MissionSeedStatus::invalidOccurrence, outputGroups, groupCount, output);
        }
        const format::Object& object = catalog.objects()[occurrence.objectIndex];
        // A mission owns what its own seed carries. Publishing a group builds that object's placed
        // content on the client, so an object the mission never addresses is its to leave out.
        bool omitted = false;
        for (const MissionSeedOmission& omission : omissions) {
            omitted = omitted
                      || (omission.objectTag == object.objectTag
                          && omission.registryKey == object.objectKey);
        }
        if (omitted) {
            continue;
        }
        const auto slots = object_slots(catalog, object);
        bool hasDescriptorBackedSlot = false;
        bool everySlotDescriptorBacked = true;
        for (const format::Slot& slot : slots) {
            if (slot.objectIndex != occurrence.objectIndex) {
                return refuse_mission_seed(
                    MissionSeedStatus::invalidOccurrence, outputGroups, groupCount, output);
            }
            if (slot.componentClass == format::kAbsentIndex) {
                everySlotDescriptorBacked = false;
                continue;
            }
            hasDescriptorBackedSlot = true;
            if ((slot.flags & format::kSlotSchemaJoinExact) == 0) {
                return refuse_mission_seed(
                    MissionSeedStatus::schemaJoinNotExact, outputGroups, groupCount, output);
            }
        }
        // A short group leaves the client a sync record that never seeds, which should hold the
        // bubble's seed commit shut. Excluding those objects publishes nothing at all here, so
        // the count is reported and the object is still published.
        if (!everySlotDescriptorBacked) {
            ++candidate.incompleteObjectCount;
        }
        if (!hasDescriptorBackedSlot) {
            continue;
        }
        // The client builds every non-replicated placed object itself on slice load, and a host
        // owes sync records only for the replicated ones. Publishing the rest makes the client
        // build a second copy and pay its one sensor heap for it.
        if (object.placedLeafCount != 0 && object.replicatedPlacementCount == 0) {
            ++candidate.unreplicatedObjectCount;
            continue;
        }

        layouts::RosterGroup group{};
        if (!materialize_roster_group(catalog, object, group)) {
            return refuse_mission_seed(
                MissionSeedStatus::invalidRosterGroup, outputGroups, groupCount, output);
        }
        bool duplicate = false;
        for (std::size_t index = 0; index < groupCount; ++index) {
            if (outputGroups[index].registryKey != group.registryKey) {
                continue;
            }
            if (!same_roster_group(outputGroups[index], group)) {
                return refuse_mission_seed(
                    MissionSeedStatus::rosterKeyConflict, outputGroups, groupCount, output);
            }
            duplicate = true;
            break;
        }
        if (duplicate) {
            continue;
        }
        if (groupCount >= outputGroups.size()) {
            return refuse_mission_seed(
                MissionSeedStatus::groupCapacityExceeded, outputGroups, groupCount, output);
        }
        outputGroups[groupCount] = group;
        ++groupCount;
        candidate.authMappingSlots += group.slotCount;
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            candidate.authResetSlots +=
                (group.slotFlags[slot] & layouts::kSlotAuthFlag) != 0 ? 1U : 0U;
            candidate.senseSuppressedSlots +=
                (group.slotFlags[slot] & layouts::kSlotSenseFlag) != 0 ? 1U : 0U;
        }
    }
    candidate.groupCount = static_cast<std::uint32_t>(groupCount);
    output = candidate;
    return MissionSeedStatus::ready;
}

/** Builds one all-or-nothing set of exact type-43 resource inputs in object-slot order. */
AuthoredSceneSeedStatus materialize_authored_scene_seeds(const Catalog& catalog,
                                                         const format::Object& object,
                                                         std::span<AuthoredSceneSeed> outputSeeds,
                                                         std::size_t& outputCount) noexcept {
    std::fill(outputSeeds.begin(), outputSeeds.end(), AuthoredSceneSeed{});
    outputCount = 0;

    const auto objects = catalog.objects();
    if (!owns(objects, object) || object.objectTag == 0 || object.objectTag == format::kAbsentIndex
        || object.objectKey == 0 || object.objectKey == format::kAbsentIndex) {
        return AuthoredSceneSeedStatus::invalidObject;
    }
    const std::uint32_t objectIndex = static_cast<std::uint32_t>(&object - objects.data());
    const auto slots = object_slots(catalog, object);

    std::size_t required = 0;
    for (const format::Slot& slot : slots) {
        if (slot.slotType != format::kAuthoredSceneSlotType
            || slot.componentClass == format::kAbsentIndex) {
            continue;
        }
        if (slot.objectIndex != objectIndex
            || slot.componentClass != format::kAuthoredSceneComponentClass
            || slot.senseSchema != format::kAuthoredSceneSenseSchema
            || slot.authSchema != format::kAuthoredSceneAuthSchema
            || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
            return AuthoredSceneSeedStatus::schemaMismatch;
        }

        const auto resources = slot_authored_scene_resources(catalog, slot);
        if (resources.empty()) {
            // A missing scene resource is not unexpected
            // Activities may contain ghost-slots where content was likely removed but the slot itself was not.
            std::array<char, 80> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=activity stage=mission_scene_seeds result=missing_resource slot=%d",
                                              slot.slotIndex);
            if (written > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            continue;
        }
        if (resources.size() != 1) {
            return AuthoredSceneSeedStatus::ambiguousResource;
        }
        const format::AuthoredSceneResource& resource = resources.front();
        if (resource.flags != format::kAuthoredSceneResourceExact
            || resource.resourceClass != format::kAuthoredSceneResourceClass
            || resource.resourceTag == 0 || resource.resourceTag == format::kAbsentIndex) {
            return AuthoredSceneSeedStatus::missingResource;
        }
        ++required;
    }
    if (required > outputSeeds.size()) {
        return AuthoredSceneSeedStatus::capacityExceeded;
    }

    std::size_t written = 0;
    for (const format::Slot& slot : slots) {
        if (slot.slotType != format::kAuthoredSceneSlotType
            || slot.componentClass == format::kAbsentIndex) {
            continue;
        }
        const format::AuthoredSceneResource& resource =
            slot_authored_scene_resources(catalog, slot).front();
        outputSeeds[written++] = {object.objectTag,
                                  object.objectKey,
                                  slot.slotIndex,
                                  slot.slotType,
                                  slot.authSchema,
                                  resource.resourceTag};
    }
    outputCount = written;
    return AuthoredSceneSeedStatus::ready;
}

} // namespace sunrise::state::activity_sdk
