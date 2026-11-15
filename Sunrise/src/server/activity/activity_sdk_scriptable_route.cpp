#include "activity_sdk_scriptable_route.h"

#include <cstddef>

namespace sunrise::server::activity::activity_sdk_scriptables {
namespace {

namespace format = state::activity_sdk::format;
namespace layouts = state::build_data::scenarios;
namespace sdk = state::activity_sdk;

/** Finds one exact Auth slot and its compressed group offset. */
[[nodiscard]] bool selected_slot(const sdk::Catalog& catalog,
                                 const format::Object& object,
                                 const SourceIdentity& source,
                                 const layouts::RosterGroup& group,
                                 std::uint16_t& output) noexcept {
    output = 0;
    const format::Slot* selected = nullptr;
    for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
        if (slot.slotIndex != source.slotIndex || slot.slotType != source.slotType) {
            continue;
        }
        if (selected != nullptr || slot.authSchema != source.authSchema
            || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
            return false;
        }
        selected = &slot;
    }
    if (selected == nullptr) {
        return false;
    }

    std::size_t matches = 0;
    for (std::size_t index = 0; index < group.slotCount; ++index) {
        if (group.slotIndices[index] == source.slotIndex
            && group.slotTypes[index] == source.slotType
            && (group.slotFlags[index] & layouts::kSlotAuthFlag) != 0) {
            output = static_cast<std::uint16_t>(index);
            ++matches;
        }
    }
    return matches == 1;
}

} // namespace

/** Resolves one exact occurrence and stages its complete group in one region. */
Status resolve(const sdk::BoundView& view,
               const SourceIdentity& source,
               std::int32_t region,
               Resolution& output) noexcept {
    output = {};
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (view.catalog == nullptr || scenario == nullptr || region < 0
        || scenario->tag != source.scenarioTag || source.objectTag == 0 || source.registryKey == 0
        || source.authSchema == 0 || source.slotType == 0) {
        return view.catalog == nullptr || scenario == nullptr ? Status::invalidView
                                                              : Status::invalidSource;
    }

    const sdk::Catalog& catalog = *view.catalog;
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    const auto objects = catalog.objects();
    if (source.objectRow >= objects.size() || source.stateRow >= states.size()) {
        return Status::invalidSource;
    }
    const format::Object& selectedSource = objects[source.objectRow];
    if (selectedSource.objectTag != source.objectTag
        || selectedSource.objectKey != source.registryKey
        || states[source.stateRow].scenarioIndex != view.scenarioRow) {
        return Status::invalidSource;
    }
    bool sourceOccurrenceFound = false;
    for (const format::Occurrence& occurrence : sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.scenarioIndex != view.scenarioRow || occurrence.stateIndex >= states.size()
            || occurrence.bubbleIndex >= bubbles.size()
            || occurrence.objectIndex >= objects.size()) {
            return Status::invalidView;
        }
        const format::State& state = states[occurrence.stateIndex];
        const format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
        if (state.scenarioIndex != view.scenarioRow || bubble.scenarioIndex != view.scenarioRow
            || state.bubbleIndex != occurrence.bubbleIndex) {
            return Status::invalidView;
        }
        if (occurrence.objectIndex == source.objectRow
            && occurrence.stateIndex == source.stateRow) {
            sourceOccurrenceFound = true;
        }
    }
    if (!sourceOccurrenceFound) {
        return Status::missingSource;
    }

    const format::Object& object = objects[source.objectRow];
    layouts::RosterGroup group{};
    std::uint16_t slotOffset = 0;
    if (!sdk::materialize_roster_group(catalog, object, group)
        || group.objectTag != source.objectTag || group.registryKey != source.registryKey
        || !selected_slot(catalog, object, source, group, slotOffset)) {
        return Status::invalidSource;
    }

    output.target.objectTag = group.objectTag;
    output.target.registryKey = group.registryKey;
    output.target.authSchema = source.authSchema;
    output.target.rosterGroupIndex = host::kGeneratedRosterGroupIndex;
    output.target.rosterSlotOffset = slotOffset;
    output.target.slotIndex = source.slotIndex;
    output.target.sdkObjectIndex = source.objectRow;
    output.target.stateLocalRegion = region;
    output.target.slotType = source.slotType;
    output.target.stateLocalRoster = true;
    output.rosterGroup = group;
    output.scenarioRow = view.scenarioRow;
    output.stateRow = source.stateRow;
    output.region = region;
    return Status::ready;
}

} // namespace sunrise::server::activity::activity_sdk_scriptables
