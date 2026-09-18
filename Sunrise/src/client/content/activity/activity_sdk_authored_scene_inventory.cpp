#include "activity_sdk_authored_scene_inventory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "activity_sdk_authored_scene_internal.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

/** Formats one resource ID from its exact descriptor tuple. */
bool resource_id(const topology::Snapshot& topology,
                 const squad::DescriptorFact& descriptor,
                 Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "authored-scene-resource/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

/** Formats one scene-to-squad edge ID from its descriptor tuple and the squad slot it names. */
bool edge_id(const topology::Snapshot& topology,
             const squad::DescriptorFact& descriptor,
             std::uint32_t squadSlotRow,
             Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size() || squadSlotRow >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    const topology::Slot& squad = topology.slots[squadSlotRow];
    return format_text(output,
                       "authored-scene-squad-edge/%08x/%08x/%08x/%04x/%04x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType),
                       static_cast<unsigned>(squad.slotIndex),
                       static_cast<unsigned>(squad.slotType));
}

/** Formats one task-to-objective target ID from its exact descriptor tuple. */
bool task_target_id(const topology::Snapshot& topology,
                    const squad::DescriptorFact& descriptor,
                    Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "task-target/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

/** Tests one exact final slot shape supplied by the separate schema join. */
bool slot_shape(const topology::Snapshot& topology,
                const SchemaIndex& schemas,
                std::uint32_t slotIndex,
                std::uint32_t slotType,
                std::uint32_t componentClass,
                std::uint32_t senseSchema,
                std::uint32_t authSchema) noexcept {
    if (slotIndex >= topology.slots.size()) {
        return false;
    }
    const auto found = schemas.find(slotIndex);
    if (found == schemas.end() || found->second == nullptr || !found->second->exact) {
        return false;
    }
    const topology::Slot& slot = topology.slots[slotIndex];
    const squad::SlotSchemaFact& schema = *found->second;
    return slot.slotType == slotType && schema.slotIndex == slotIndex
           && schema.componentClass == componentClass && schema.senseSchema == senseSchema
           && schema.authSchema == authSchema;
}

/** Builds and validates the unique global schema lookup. */
bool schema_index(const topology::Snapshot& topology, const Facts& facts, SchemaIndex& output) {
    output.clear();
    try {
        output.reserve(facts.slotSchemas.size());
        for (const squad::SlotSchemaFact& schema : facts.slotSchemas) {
            if (schema.slotIndex >= topology.slots.size()
                || !output.emplace(schema.slotIndex, &schema).second) {
                return false;
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Checks the topology fields consumed by this bounded projection. */
bool valid_topology(const topology::Snapshot& topology) noexcept {
    if (!topology.ready || topology.objects.empty() || topology.slots.empty()) {
        return false;
    }
    std::size_t nextSlot = 0;
    for (std::size_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
        const topology::Object& object = topology.objects[objectIndex];
        if (object.objectTag == 0 || object.objectTag == format::kAbsentIndex
            || object.objectKey == 0 || object.objectKey == format::kAbsentIndex
            || object.firstSlot != nextSlot || object.firstSlot > topology.slots.size()
            || object.slotCount > topology.slots.size() - object.firstSlot) {
            return false;
        }
        for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
             ++index) {
            const topology::Slot& slot = topology.slots[index];
            if (slot.objectIndex != objectIndex || slot.slotIndex != index - object.firstSlot) {
                return false;
            }
        }
        nextSlot += object.slotCount;
    }
    return nextSlot == topology.slots.size();
}

namespace {

namespace tables = middleware::content::packages::tables;

/** The target slot type follows the object key in a scene reference. */
constexpr std::size_t kTargetSlotTypeRelativeOffset = 4U;
/** The target slot index follows the target slot type. */
constexpr std::size_t kTargetSlotIndexRelativeOffset = 6U;

/** One cached package entry and its physical class. */
struct PackageRow final {
    std::vector<std::byte> bytes{};
    std::uint32_t classId{};
};

/** Package reads are cached by tag for one transaction. */
using PackageCache = std::unordered_map<std::uint32_t, PackageRow>;

/** @return True when one complete range lies in the package blob. */
[[nodiscard]] bool
contains(std::span<const std::byte> blob, std::size_t offset, std::size_t size) noexcept {
    return offset <= blob.size() && size <= blob.size() - offset;
}

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    output = {};
    if (!contains(blob, offset, sizeof output)) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

/** Names the guard that refused one type-42 sensor, so one run can say why an idle has no state. */
void log_performance_edge(const squad::DescriptorFact& descriptor, const char* result) noexcept {
    std::array<char, 176> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_sdk_performance_edge result=%s config=0x%08X offset=0x%X "
                      "slot_row=%u",
                      result,
                      static_cast<unsigned>(descriptor.configTag),
                      static_cast<unsigned>(descriptor.descriptorOffset),
                      static_cast<unsigned>(descriptor.slotIndex));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::debug,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Logs a scene descriptor that produced no resource row, and why. */
void log_scene_resource(const squad::DescriptorFact& descriptor,
                        std::uint32_t resourceTag,
                        const char* result,
                        core::log::Level level) noexcept {
    std::array<char, 176> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_sdk_scene_resource result=%s config=0x%08X offset=0x%X "
                      "slot_row=%u resource=0x%08X",
                      result,
                      static_cast<unsigned>(descriptor.configTag),
                      static_cast<unsigned>(descriptor.descriptorOffset),
                      static_cast<unsigned>(descriptor.slotIndex),
                      static_cast<unsigned>(resourceTag));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            level,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Reads one tag once and retains the physical class beside its bytes. */
[[nodiscard]] bool package_row(squad::TagReader reader,
                               void* readerContext,
                               std::uint32_t tag,
                               PackageCache& cache,
                               const PackageRow*& output) {
    output = nullptr;
    const auto found = cache.find(tag);
    if (found != cache.end()) {
        output = &found->second;
        return true;
    }
    PackageRow row{};
    if (tag == 0 || tag == format::kAbsentIndex
        || !reader(readerContext, tag, row.bytes, row.classId)) {
        return false;
    }
    const auto [inserted, accepted] = cache.emplace(tag, std::move(row));
    if (!accepted) {
        return false;
    }
    output = &inserted->second;
    return true;
}

/** Finds one globally unique target slot by the authored client-reference triple. */
[[nodiscard]] bool target_slot(const topology::Snapshot& topology,
                               const squad::DescriptorFact& descriptor,
                               std::uint32_t targetObjectKey,
                               std::uint16_t targetSlotType,
                               std::uint16_t targetSlotIndex,
                               std::uint32_t& output,
                               bool& found) noexcept {
    output = format::kAbsentIndex;
    found = false;
    if (descriptor.objectIndex >= topology.objects.size()) {
        return false;
    }
    for (std::uint32_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
        const topology::Object& object = topology.objects[objectIndex];
        if (object.objectKey != targetObjectKey) {
            continue;
        }
        if (object.firstSlot > topology.slots.size()
            || object.slotCount > topology.slots.size() - object.firstSlot) {
            return false;
        }
        for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
             ++index) {
            const topology::Slot& slot = topology.slots[index];
            if (slot.objectIndex != objectIndex || slot.slotType != targetSlotType
                || slot.slotIndex != targetSlotIndex) {
                continue;
            }
            if (found) {
                output = format::kAbsentIndex;
                found = false;
                return true;
            }
            output = index;
            found = true;
        }
    }
    return true;
}

/** Finds one target slot inside the descriptor's owning object. */
[[nodiscard]] bool same_object_slot(const topology::Snapshot& topology,
                                    const squad::DescriptorFact& descriptor,
                                    std::uint16_t targetSlotType,
                                    std::uint16_t targetSlotIndex,
                                    std::uint32_t& output) noexcept {
    output = format::kAbsentIndex;
    if (descriptor.objectIndex >= topology.objects.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    if (object.firstSlot > topology.slots.size()
        || object.slotCount > topology.slots.size() - object.firstSlot) {
        return false;
    }
    for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
         ++index) {
        const topology::Slot& slot = topology.slots[index];
        if (slot.objectIndex == descriptor.objectIndex && slot.slotType == targetSlotType
            && slot.slotIndex == targetSlotIndex) {
            output = index;
            return true;
        }
    }
    return true;
}

} // namespace

/** Builds both authored-scene sections from complete topology and package facts. */
bool build(const topology::Snapshot& topology,
           const Facts& facts,
           squad::TagReader reader,
           void* readerContext,
           Snapshot& output) noexcept {
    output = {};
    if (reader == nullptr || !valid_topology(topology)) {
        return false;
    }
    try {
        SchemaIndex schemas{};
        PackageCache cache{};
        Snapshot pending{};
        if (!schema_index(topology, facts, schemas)) {
            return false;
        }
        const std::size_t sceneDescriptorCount = static_cast<std::size_t>(std::count_if(
            facts.descriptors.begin(), facts.descriptors.end(), [&topology](const auto& row) {
                return is_scene_descriptor(topology, row);
            }));
        cache.reserve(sceneDescriptorCount);
        pending.resources.reserve(sceneDescriptorCount);
        pending.squadEdges.reserve(sceneDescriptorCount);
        pending.taskTargets.reserve(facts.descriptors.size() - sceneDescriptorCount);
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            if (is_task_descriptor(topology, descriptor)) {
                if (!slot_shape(topology,
                                schemas,
                                descriptor.slotIndex,
                                format::kTaskSlotType,
                                format::kTaskComponentClass,
                                format::kAbsentIndex,
                                format::kTaskAuthSchema)) {
                    continue;
                }
                const PackageRow* config = nullptr;
                if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                    || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                    continue;
                }
                const auto blob = std::span(config->bytes);
                const std::size_t referenceField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kTaskReferenceRelativeOffset;
                const std::size_t bitIndexField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kTaskBitIndexRelativeOffset;
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                std::uint32_t bitIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)
                    || !read_value(blob, bitIndexField, bitIndex)) {
                    return false;
                }
                if (targetSlotType == format::kObjectiveSlotType && bitIndex < 24U) {
                    std::uint32_t linkedSlot = format::kAbsentIndex;
                    bool found = false;
                    if (!target_slot(topology,
                                     descriptor,
                                     targetObjectKey,
                                     targetSlotType,
                                     targetSlotIndex,
                                     linkedSlot,
                                     found)) {
                        return false;
                    }
                    if (found
                        && slot_shape(topology,
                                      schemas,
                                      linkedSlot,
                                      format::kObjectiveSlotType,
                                      format::kObjectiveComponentClass,
                                      format::kObjectiveSenseSchema,
                                      format::kObjectiveAuthSchema)) {
                        TaskTarget row{};
                        if (!task_target_id(topology, descriptor, row.id)) {
                            continue;
                        }
                        row.taskSlotIndex = descriptor.slotIndex;
                        row.objectiveSlotIndex = linkedSlot;
                        row.configTag = descriptor.configTag;
                        row.descriptorOffset = descriptor.descriptorOffset;
                        row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
                        row.targetObjectKey = targetObjectKey;
                        row.bitIndex = bitIndex;
                        row.flags = format::kTaskTargetExact;
                        pending.taskTargets.push_back(row);
                    }
                }
            }
            if (is_performance_descriptor(topology, descriptor)) {
                // A type-42 sensor drives the same-object squad named at descriptor +0x58.
                if (!slot_shape(topology,
                                schemas,
                                descriptor.slotIndex,
                                format::kPerformanceSlotType,
                                format::kPerformanceComponentClass,
                                format::kAbsentIndex,
                                format::kPerformanceAuthSchema)) {
                    log_performance_edge(descriptor, "sensor_shape");
                    continue;
                }
                const PackageRow* config = nullptr;
                if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                    || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                    log_performance_edge(descriptor, "config_unreadable");
                    continue;
                }
                const auto blob = std::span(config->bytes);
                const std::size_t referenceField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kPerformanceSquadReferenceRelativeOffset;
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)
                    || targetSlotType != format::kSquadSlotType
                    || descriptor.objectIndex >= topology.objects.size()
                    || targetObjectKey != topology.objects[descriptor.objectIndex].objectKey) {
                    log_performance_edge(descriptor, "reference");
                    continue;
                }
                std::uint32_t linkedSlot = format::kAbsentIndex;
                if (!same_object_slot(
                        topology, descriptor, targetSlotType, targetSlotIndex, linkedSlot)) {
                    return false;
                }
                if (linkedSlot == format::kAbsentIndex
                    || !slot_shape(topology,
                                   schemas,
                                   linkedSlot,
                                   format::kSquadSlotType,
                                   format::kSquadComponentClass,
                                   format::kSquadSenseSchema,
                                   format::kSquadAuthSchema)) {
                    log_performance_edge(descriptor, "target_shape");
                    continue;
                }
                SquadEdge row{};
                if (!edge_id(topology, descriptor, linkedSlot, row.id)) {
                    log_performance_edge(descriptor, "identity");
                    continue;
                }
                row.sceneSlotIndex = descriptor.slotIndex;
                row.squadSlotIndex = linkedSlot;
                row.configTag = descriptor.configTag;
                row.descriptorOffset = descriptor.descriptorOffset;
                row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
                row.targetObjectKey = targetObjectKey;
                row.flags = format::kAuthoredSceneSquadPerformanceTargetExact;
                pending.squadEdges.push_back(row);
                log_performance_edge(descriptor, "ready");
                continue;
            }
            if (!is_scene_descriptor(topology, descriptor)) {
                continue;
            }
            if (!slot_shape(topology,
                            schemas,
                            descriptor.slotIndex,
                            format::kAuthoredSceneSlotType,
                            format::kAuthoredSceneComponentClass,
                            format::kAuthoredSceneSenseSchema,
                            format::kAuthoredSceneAuthSchema)) {
                continue;
            }
            const PackageRow* config = nullptr;
            if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                continue;
            }
            const auto blob = std::span(config->bytes);
            const std::size_t resourceField = static_cast<std::size_t>(descriptor.descriptorOffset)
                                              + format::kAuthoredSceneResourceRelativeOffset;
            std::uint32_t resourceTag = 0;
            if (!read_value(blob, resourceField, resourceTag)) {
                continue;
            }
            if (resourceTag == 0 || resourceTag == format::kAbsentIndex) {
                log_scene_resource(descriptor, resourceTag, "unresourced", core::log::Level::debug);
                pending.unresourcedSlots.push_back(descriptor.slotIndex);
            } else {
                const PackageRow* resourcePackage = nullptr;
                if (!package_row(reader, readerContext, resourceTag, cache, resourcePackage)
                    || resourcePackage == nullptr) {
                    log_scene_resource(
                        descriptor, resourceTag, "unreadable", core::log::Level::warn);
                } else if (resourcePackage->classId != format::kAuthoredSceneResourceClass) {
                    log_scene_resource(descriptor, resourceTag, "class", core::log::Level::warn);
                } else {
                    Resource row{};
                    if (resource_id(topology, descriptor, row.id)) {
                        row.slotIndex = descriptor.slotIndex;
                        row.configTag = descriptor.configTag;
                        row.descriptorOffset = descriptor.descriptorOffset;
                        row.resourceFieldOffset = static_cast<std::uint32_t>(resourceField);
                        row.resourceTag = resourceTag;
                        row.resourceClass = resourcePackage->classId;
                        row.flags = format::kAuthoredSceneResourceExact;
                        pending.resources.push_back(row);
                    }
                }
            }

            // Every squad in the participant table is an edge; the other participant kinds
            // (point sets, objects) are not.
            const std::size_t descriptorOffset = descriptor.descriptorOffset;
            std::uint64_t participantCount = 0;
            std::uint32_t tableClass = 0;
            if (!read_value(blob,
                            descriptorOffset + format::kAuthoredSceneParticipantCountRelativeOffset,
                            participantCount)
                || !read_value(blob,
                               descriptorOffset
                                   + format::kAuthoredSceneParticipantTableClassRelativeOffset,
                               tableClass)
                || tableClass != format::kAuthoredSceneParticipantTableClass
                || participantCount > format::kAuthoredSceneParticipantCapacity) {
                continue;
            }
            for (std::uint64_t entry = 0; entry < participantCount; ++entry) {
                const std::size_t pointerField =
                    descriptorOffset + format::kAuthoredSceneParticipantTableRelativeOffset
                    + static_cast<std::size_t>(entry)
                          * format::kAuthoredSceneParticipantPointerSize;
                std::uint64_t pointer = 0;
                if (!read_value(blob, pointerField, pointer)
                    || pointer > blob.size() - pointerField) {
                    break;
                }
                const std::size_t payload = pointerField + static_cast<std::size_t>(pointer);
                std::uint32_t blockClass = 0;
                if (payload < format::kAuthoredSceneParticipantClassSize
                    || !read_value(
                        blob, payload - format::kAuthoredSceneParticipantClassSize, blockClass)) {
                    break;
                }
                if (blockClass != format::kAuthoredSceneSquadBlockClass) {
                    continue;
                }
                const std::size_t referenceField =
                    payload + format::kAuthoredSceneSquadPayloadReferenceOffset;
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)) {
                    break;
                }
                if (targetSlotType != format::kSquadSlotType
                    || descriptor.objectIndex >= topology.objects.size()
                    || targetObjectKey != topology.objects[descriptor.objectIndex].objectKey) {
                    continue;
                }
                std::uint32_t linkedSlot = format::kAbsentIndex;
                if (!same_object_slot(
                        topology, descriptor, targetSlotType, targetSlotIndex, linkedSlot)) {
                    return false;
                }
                if (linkedSlot == format::kAbsentIndex
                    || !slot_shape(topology,
                                   schemas,
                                   linkedSlot,
                                   format::kSquadSlotType,
                                   format::kSquadComponentClass,
                                   format::kSquadSenseSchema,
                                   format::kSquadAuthSchema)) {
                    continue;
                }
                SquadEdge row{};
                if (!edge_id(topology, descriptor, linkedSlot, row.id)) {
                    continue;
                }
                row.sceneSlotIndex = descriptor.slotIndex;
                row.squadSlotIndex = linkedSlot;
                row.configTag = descriptor.configTag;
                row.descriptorOffset = descriptor.descriptorOffset;
                row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
                row.targetObjectKey = targetObjectKey;
                row.flags = format::kAuthoredSceneSquadSameObjectExact;
                pending.squadEdges.push_back(row);
            }
        }
        // A sensor with no descriptor fact logs nothing above, so count both sides here.
        {
            const std::size_t sensorSlots = static_cast<std::size_t>(
                std::count_if(topology.slots.begin(), topology.slots.end(), [](const auto& slot) {
                    return slot.slotType == format::kPerformanceSlotType;
                }));
            const std::size_t sensorDescriptors = static_cast<std::size_t>(std::count_if(
                facts.descriptors.begin(), facts.descriptors.end(), [&topology](const auto& row) {
                    return is_performance_descriptor(topology, row);
                }));
            std::array<char, 160> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=activity_sdk_performance_edge stage=summary "
                                              "sensor_slots=%zu sensor_descriptors=%zu",
                                              sensorSlots,
                                              sensorDescriptors);
            if (written > 0) {
                core::log::write(
                    core::log::Channel::client,
                    core::log::Level::info,
                    {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
            }
        }
        std::sort(pending.resources.begin(), pending.resources.end(), resource_less);
        std::sort(pending.squadEdges.begin(), pending.squadEdges.end(), edge_less);
        std::sort(pending.taskTargets.begin(), pending.taskTargets.end(), task_less);
        pending.resources.erase(std::unique(pending.resources.begin(),
                                            pending.resources.end(),
                                            [](const auto& left, const auto& right) {
                                                return resource_natural(left)
                                                       == resource_natural(right);
                                            }),
                                pending.resources.end());
        pending.squadEdges.erase(std::unique(pending.squadEdges.begin(),
                                             pending.squadEdges.end(),
                                             [](const auto& left, const auto& right) {
                                                 return edge_natural(left) == edge_natural(right);
                                             }),
                                 pending.squadEdges.end());
        pending.taskTargets.erase(std::unique(pending.taskTargets.begin(),
                                              pending.taskTargets.end(),
                                              [](const auto& left, const auto& right) {
                                                  return task_natural(left) == task_natural(right);
                                              }),
                                  pending.taskTargets.end());
        std::sort(pending.unresourcedSlots.begin(), pending.unresourcedSlots.end());
        pending.unresourcedSlots.erase(
            std::unique(pending.unresourcedSlots.begin(), pending.unresourcedSlots.end()),
            pending.unresourcedSlots.end());
        pending.complete = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
