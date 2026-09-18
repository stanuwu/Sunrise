#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "activity_sdk_authored_scene_internal.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {
namespace {

/** Hash combining uses the 32-bit golden-ratio increment. */
constexpr std::size_t kHashCombineConstant = 0x9E3779B9U;

/** Derives the descriptor identity used by every authored-scene child row. */
[[nodiscard]] bool descriptor_id(const topology::Snapshot& topology,
                                 const squad::DescriptorFact& descriptor,
                                 std::string& output) {
    output.clear();
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    if (slot.objectIndex != descriptor.objectIndex) {
        return false;
    }
    std::array<char, 80> buffer{};
    const int written = std::snprintf(buffer.data(),
                                      buffer.size(),
                                      "descriptor/%08x/%08x/%08x/%04x/%04x",
                                      static_cast<unsigned>(descriptor.configTag),
                                      static_cast<unsigned>(object.objectTag),
                                      static_cast<unsigned>(descriptor.descriptorOffset),
                                      static_cast<unsigned>(slot.slotIndex),
                                      static_cast<unsigned>(slot.slotType));
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return false;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(written));
    return true;
}

/** Checks the complete descriptor set before package reads begin. */
[[nodiscard]] bool valid_facts(const topology::Snapshot& topology, const Facts& facts) {
    if (!facts.complete || facts.slotSchemas.size() != topology.slots.size()) {
        return false;
    }
    std::unordered_set<std::string> ids{};
    std::unordered_set<std::uint32_t> schemaSlots{};
    try {
        ids.reserve(facts.descriptors.size());
        schemaSlots.reserve(facts.slotSchemas.size());
        std::vector<std::size_t> descriptorCounts(topology.objects.size());
        std::vector<bool> projectedOwners(topology.objects.size());
        for (const squad::SlotSchemaFact& schema : facts.slotSchemas) {
            if (schema.slotIndex >= topology.slots.size()
                || !schemaSlots.emplace(schema.slotIndex).second) {
                return false;
            }
            const topology::Slot& slot = topology.slots[schema.slotIndex];
            if (slot.objectIndex >= topology.objects.size()) {
                return false;
            }
            if (slot.slotType == format::kAuthoredSceneSlotType && schema.exact
                && schema.componentClass == format::kAuthoredSceneComponentClass
                && schema.senseSchema == format::kAuthoredSceneSenseSchema
                && schema.authSchema == format::kAuthoredSceneAuthSchema) {
                projectedOwners[slot.objectIndex] = true;
            }
            if (slot.slotType == format::kTaskSlotType && schema.exact
                && schema.componentClass == format::kTaskComponentClass
                && schema.senseSchema == format::kAbsentIndex
                && schema.authSchema == format::kTaskAuthSchema) {
                projectedOwners[slot.objectIndex] = true;
            }
        }
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            std::string expected{};
            if (!descriptor.complete || descriptor.configTag == 0
                || descriptor.configTag == format::kAbsentIndex
                || descriptor.objectIndex >= topology.objects.size()
                || descriptor.slotIndex >= topology.slots.size()
                || descriptor.descriptorOffset
                       > format::kAbsentIndex - format::kAuthoredSceneSquadReferenceRelativeOffset
                || !descriptor_id(topology, descriptor, expected) || descriptor.id != expected
                || !ids.emplace(descriptor.id).second) {
                return false;
            }
            ++descriptorCounts[descriptor.objectIndex];
        }
        for (std::size_t index = 0; index < topology.objects.size(); ++index) {
            const topology::Object& object = topology.objects[index];
            if (projectedOwners[index]
                && (!object.descriptorEvidenceComplete
                    || object.descriptorCount == format::kAbsentIndex
                    || descriptorCounts[index] != object.descriptorCount)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** One descriptor lookup key matches the final natural identity. */
struct DescriptorKey final {
    std::uint32_t slotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};

    bool operator==(const DescriptorKey&) const = default;
};

/** Mixes the three exact u32 identity lanes without narrowing them. */
struct DescriptorKeyHash final {
    [[nodiscard]] std::size_t operator()(const DescriptorKey& value) const noexcept {
        std::size_t result = value.slotIndex;
        result ^= static_cast<std::size_t>(value.configTag) + kHashCombineConstant + (result << 6U)
                  + (result >> 2U);
        result ^= static_cast<std::size_t>(value.descriptorOffset) + kHashCombineConstant
                  + (result << 6U) + (result >> 2U);
        return result;
    }
};

using DescriptorIndex =
    std::unordered_map<DescriptorKey, const squad::DescriptorFact*, DescriptorKeyHash>;

/** Builds the exact descriptor lookup consumed by output validation. */
[[nodiscard]] bool descriptor_index(const Facts& facts, DescriptorIndex& output) {
    output.clear();
    try {
        output.reserve(facts.descriptors.size());
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            const DescriptorKey key{
                descriptor.slotIndex, descriptor.configTag, descriptor.descriptorOffset};
            if (!output.emplace(key, &descriptor).second) {
                return false;
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Finds one descriptor fact by its natural package identity. */
[[nodiscard]] const squad::DescriptorFact*
find_descriptor(const DescriptorIndex& descriptors,
                std::uint32_t slotIndex,
                std::uint32_t configTag,
                std::uint32_t descriptorOffset) noexcept {
    const auto found = descriptors.find({slotIndex, configTag, descriptorOffset});
    return found == descriptors.end() ? nullptr : found->second;
}

} // namespace

/** Validates every structural ID, scalar domain, slot join, flag, and row order. */
bool validate(const topology::Snapshot& topology,
              const Facts& facts,
              const Snapshot& snapshot) noexcept {
    if (!snapshot.complete || !valid_topology(topology) || !valid_facts(topology, facts)) {
        return false;
    }
    SchemaIndex schemas{};
    DescriptorIndex descriptors{};
    if (!schema_index(topology, facts, schemas) || !descriptor_index(facts, descriptors)) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.resources.size(); ++index) {
        const Resource& row = snapshot.resources[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.slotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        if (descriptor == nullptr || !is_scene_descriptor(topology, *descriptor)
            || !resource_id(topology, *descriptor, expectedId) || expectedId.value != row.id.value
            || expectedId.length != row.id.length
            || !slot_shape(topology,
                           schemas,
                           row.slotIndex,
                           format::kAuthoredSceneSlotType,
                           format::kAuthoredSceneComponentClass,
                           format::kAuthoredSceneSenseSchema,
                           format::kAuthoredSceneAuthSchema)
            || row.descriptorOffset
                   > format::kAbsentIndex - format::kAuthoredSceneResourceRelativeOffset
            || row.resourceFieldOffset
                   != row.descriptorOffset + format::kAuthoredSceneResourceRelativeOffset
            || row.resourceTag == 0 || row.resourceTag == format::kAbsentIndex
            || row.resourceClass != format::kAuthoredSceneResourceClass
            || row.flags != format::kAuthoredSceneResourceExact || row.reserved != 0
            || (index != 0 && !resource_less(snapshot.resources[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.resources.size(); ++index) {
        if (resource_natural(snapshot.resources[index - 1])
            == resource_natural(snapshot.resources[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.eventKeys.size(); ++index) {
        const EventKey& row = snapshot.eventKeys[index];
        // A key belongs to a resourced scene and names the resource its graph hangs off.
        const auto resource = std::find_if(
            snapshot.resources.begin(),
            snapshot.resources.end(),
            [&row](const Resource& candidate) { return candidate.slotIndex == row.slotIndex; });
        const squad::DescriptorFact* descriptor = resource == snapshot.resources.end()
                                                      ? nullptr
                                                      : find_descriptor(descriptors,
                                                                        resource->slotIndex,
                                                                        resource->configTag,
                                                                        resource->descriptorOffset);
        Text expectedId{};
        if (descriptor == nullptr || resource->resourceTag != row.resourceTag || row.graphTag == 0
            || row.graphTag == format::kAbsentIndex || row.key == 0
            || row.key == format::kAbsentIndex || row.flags != format::kAuthoredSceneEventKeyExact
            || row.reserved != 0
            || !event_key_id(topology, *descriptor, row.graphTag, row.gateOffset, expectedId)
            || expectedId.value != row.id.value || expectedId.length != row.id.length
            || (index != 0
                && event_key_natural(snapshot.eventKeys[index - 1]) >= event_key_natural(row))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.squadEdges.size(); ++index) {
        const SquadEdge& row = snapshot.squadEdges[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.sceneSlotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        // A row is a type-43 scene edge or a type-42 performance edge; the flag says which.
        const bool performance = row.flags == format::kAuthoredSceneSquadPerformanceTargetExact;
        const std::uint32_t referenceOffset =
            performance ? format::kPerformanceSquadReferenceRelativeOffset
                        : format::kAuthoredSceneSquadReferenceRelativeOffset;
        const bool sourceShape = performance ? slot_shape(topology,
                                                          schemas,
                                                          row.sceneSlotIndex,
                                                          format::kPerformanceSlotType,
                                                          format::kPerformanceComponentClass,
                                                          format::kAbsentIndex,
                                                          format::kPerformanceAuthSchema)
                                             : slot_shape(topology,
                                                          schemas,
                                                          row.sceneSlotIndex,
                                                          format::kAuthoredSceneSlotType,
                                                          format::kAuthoredSceneComponentClass,
                                                          format::kAuthoredSceneSenseSchema,
                                                          format::kAuthoredSceneAuthSchema);
        if (descriptor == nullptr || descriptor->objectIndex >= topology.objects.size()
            || row.squadSlotIndex >= topology.slots.size()
            || topology.slots[row.squadSlotIndex].objectIndex != descriptor->objectIndex
            || !(performance ? is_performance_descriptor(topology, *descriptor)
                             : is_scene_descriptor(topology, *descriptor))
            || !edge_id(topology, *descriptor, expectedId) || expectedId.value != row.id.value
            || expectedId.length != row.id.length || !sourceShape
            || !slot_shape(topology,
                           schemas,
                           row.squadSlotIndex,
                           format::kSquadSlotType,
                           format::kSquadComponentClass,
                           format::kSquadSenseSchema,
                           format::kSquadAuthSchema)
            || row.descriptorOffset > format::kAbsentIndex - referenceOffset
            || row.referenceFieldOffset != row.descriptorOffset + referenceOffset
            || row.targetObjectKey != topology.objects[descriptor->objectIndex].objectKey
            || (row.flags != format::kAuthoredSceneSquadSameObjectExact && !performance)
            || row.reserved != 0
            || (index != 0 && !edge_less(snapshot.squadEdges[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.squadEdges.size(); ++index) {
        if (edge_natural(snapshot.squadEdges[index - 1])
            == edge_natural(snapshot.squadEdges[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.taskTargets.size(); ++index) {
        const TaskTarget& row = snapshot.taskTargets[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.taskSlotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        if (descriptor == nullptr || descriptor->objectIndex >= topology.objects.size()
            || row.objectiveSlotIndex >= topology.slots.size()
            || !is_task_descriptor(topology, *descriptor)
            || !task_target_id(topology, *descriptor, expectedId)
            || expectedId.value != row.id.value || expectedId.length != row.id.length
            || !slot_shape(topology,
                           schemas,
                           row.taskSlotIndex,
                           format::kTaskSlotType,
                           format::kTaskComponentClass,
                           format::kAbsentIndex,
                           format::kTaskAuthSchema)
            || !slot_shape(topology,
                           schemas,
                           row.objectiveSlotIndex,
                           format::kObjectiveSlotType,
                           format::kObjectiveComponentClass,
                           format::kObjectiveSenseSchema,
                           format::kObjectiveAuthSchema)
            || row.descriptorOffset > format::kAbsentIndex - format::kTaskBitIndexRelativeOffset
            || row.referenceFieldOffset
                   != row.descriptorOffset + format::kTaskReferenceRelativeOffset
            || topology.slots[row.objectiveSlotIndex].objectIndex >= topology.objects.size()
            || row.targetObjectKey
                   != topology.objects[topology.slots[row.objectiveSlotIndex].objectIndex].objectKey
            || row.bitIndex >= 24U || row.flags != format::kTaskTargetExact || row.reserved != 0
            || (index != 0 && !task_less(snapshot.taskTargets[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.taskTargets.size(); ++index) {
        if (task_natural(snapshot.taskTargets[index - 1])
            == task_natural(snapshot.taskTargets[index])) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
