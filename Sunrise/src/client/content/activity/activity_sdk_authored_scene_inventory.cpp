#include "activity_sdk_authored_scene_inventory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/container_framing.h"
#include "../../../middleware/content/packages/reader/typed_array.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808062.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808063.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808083.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808095.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "activity_sdk_authored_scene_internal.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

namespace {

namespace tables = middleware::content::packages::tables;
namespace reader = middleware::content::packages::reader;

/** The target slot type follows the object key in a scene reference. */
constexpr std::size_t kTargetSlotTypeRelativeOffset =
    offsetof(generated::TaskSlotDefinitionType38, targetSlotType)
    - offsetof(generated::TaskSlotDefinitionType38, targetObjectKey);
/** The target slot index follows the target slot type. */
constexpr std::size_t kTargetSlotIndexRelativeOffset =
    offsetof(generated::TaskSlotDefinitionType38, targetSlotIndex)
    - offsetof(generated::TaskSlotDefinitionType38, targetObjectKey);
// A performance sensor and a scene squad block place their object key and slot words like a task.
static_assert(offsetof(generated::PerformanceSlotDefinitionType42, squadSlotType)
                  - offsetof(generated::PerformanceSlotDefinitionType42, squadObjectKey)
              == kTargetSlotTypeRelativeOffset);
static_assert(offsetof(generated::PerformanceSlotDefinitionType42, squadSlotIndex)
                  - offsetof(generated::PerformanceSlotDefinitionType42, squadObjectKey)
              == kTargetSlotIndexRelativeOffset);
static_assert(offsetof(generated::SceneSquadBlock, targetSlotType)
                  - offsetof(generated::SceneSquadBlock, targetObjectKey)
              == kTargetSlotTypeRelativeOffset);
static_assert(offsetof(generated::SceneSquadBlock, targetSlotIndex)
                  - offsetof(generated::SceneSquadBlock, targetObjectKey)
              == kTargetSlotIndexRelativeOffset);

/**
 * Finds the participant rows of one scene slot definition.
 * @param blob Config bytes.
 * @param descriptorOffset Start of the scene slot definition.
 * @param rows Receives the offset of the first row; zero for an empty list.
 * @param count Receives the row count.
 * @return False when the participant field is not a table of participant rows.
 */
[[nodiscard]] bool participant_rows(std::span<const std::byte> blob,
                                    std::size_t descriptorOffset,
                                    std::size_t& rows,
                                    std::uint64_t& count) noexcept {
    using Descriptor = generated::AuthoredSceneSlotDescriptor;
    using Row = generated::SceneParticipantTable;
    const std::size_t countField = descriptorOffset + offsetof(Descriptor, participants.count);
    const std::size_t relativeField =
        descriptorOffset + offsetof(Descriptor, participants.relative);
    std::int64_t relative = 0;
    rows = 0;
    if (!read_value(blob, countField, count) || !read_value(blob, relativeField, relative)) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    // The header offset counts from the relative field itself.
    const auto header = static_cast<std::int64_t>(relativeField) + relative;
    reader::TypedArrayRows table{};
    if (header < 0 || static_cast<std::uint64_t>(header) > blob.size()
        || !reader::read_typed_array(
            blob, static_cast<std::size_t>(header), count, Row::kClass, sizeof(Row), table)) {
        return false;
    }
    rows = table.data;
    return true;
}

/** Logs the outcome of one type-42 sensor edge: the refusing guard, or `ready`. */
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

/** Reads one tag once and retains the physical class beside its bytes. */
bool package_row(squad::TagReader reader,
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
                                generated::TaskComponent::kClass,
                                format::kAbsentIndex,
                                generated::C80807d89::kClass)) {
                    continue;
                }
                const PackageRow* config = nullptr;
                if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                    || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                    continue;
                }
                const auto blob = std::span(config->bytes);
                const std::size_t referenceField =
                    static_cast<std::size_t>(descriptor.descriptorOffset) + kTaskReferenceOffset;
                const std::size_t bitIndexField =
                    static_cast<std::size_t>(descriptor.descriptorOffset) + kTaskBitIndexOffset;
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
                                      generated::ObjectiveComponentType3::kClass,
                                      generated::C80807f04::kClass,
                                      generated::C80807f0c::kClass)) {
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
                // A type-42 sensor drives the same-object squad its reference names.
                if (!slot_shape(topology,
                                schemas,
                                descriptor.slotIndex,
                                format::kPerformanceSlotType,
                                generated::PerformanceSensorComponent::kClass,
                                format::kAbsentIndex,
                                generated::C80809586::kClass)) {
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
                    + kPerformanceSquadReferenceOffset;
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
                                   generated::SSquadComponent::kClass,
                                   generated::C80807ecc::kClass,
                                   generated::C80807ec9::kClass)) {
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
                            generated::AuthoredSceneComponentType43::kClass,
                            generated::C8080626a::kClass,
                            generated::C8080626b::kClass)) {
                continue;
            }
            const PackageRow* config = nullptr;
            if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                continue;
            }
            const auto blob = std::span(config->bytes);
            const std::size_t resourceField =
                static_cast<std::size_t>(descriptor.descriptorOffset) + kSceneResourceOffset;
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
                } else if (resourcePackage->classId != generated::SEntity::kClass) {
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
                    if (!collect_event_keys(topology,
                                            descriptor,
                                            reader,
                                            readerContext,
                                            cache,
                                            resourceTag,
                                            std::span(resourcePackage->bytes),
                                            pending.eventKeys)) {
                        return false;
                    }
                }
            }

            // Every squad in the participant table is an edge; the other participant kinds
            // (point sets, objects) are not.
            std::size_t participantRows = 0;
            std::uint64_t participantCount = 0;
            if (!participant_rows(
                    blob, descriptor.descriptorOffset, participantRows, participantCount)) {
                log_scene_resource(
                    descriptor, resourceTag, "participant_table", core::log::Level::warn);
                continue;
            }
            if (participantCount > format::kAuthoredSceneParticipantCapacity) {
                log_scene_resource(
                    descriptor, resourceTag, "participant_capacity", core::log::Level::warn);
                continue;
            }
            for (std::uint64_t entry = 0; entry < participantCount; ++entry) {
                const std::size_t pointerField =
                    participantRows
                    + static_cast<std::size_t>(entry) * sizeof(generated::SceneParticipantTable)
                    + offsetof(generated::SceneParticipantTable, participant);
                std::uint64_t pointer = 0;
                std::uint32_t blockClass = 0;
                // A pointer or block that does not read is a misread table; the rest is dropped.
                if (!read_value(blob, pointerField, pointer) || pointer > blob.size() - pointerField
                    || pointerField + pointer < reader::layout::kClassWordSize
                    || !read_value(blob,
                                   pointerField + static_cast<std::size_t>(pointer)
                                       - reader::layout::kClassWordSize,
                                   blockClass)) {
                    log_scene_resource(
                        descriptor, resourceTag, "participant", core::log::Level::warn);
                    break;
                }
                if (blockClass != generated::SceneSquadBlock::kClass) {
                    continue;
                }
                const std::size_t referenceField =
                    pointerField + static_cast<std::size_t>(pointer)
                    + offsetof(generated::SceneSquadBlock, targetObjectKey);
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)) {
                    log_scene_resource(
                        descriptor, resourceTag, "squad_block", core::log::Level::warn);
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
                                   generated::SSquadComponent::kClass,
                                   generated::C80807ecc::kClass,
                                   generated::C80807ec9::kClass)) {
                    log_scene_resource(
                        descriptor, resourceTag, "squad_unrunnable", core::log::Level::debug);
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
        std::sort(pending.eventKeys.begin(), pending.eventKeys.end(), event_key_less);
        std::sort(pending.squadEdges.begin(), pending.squadEdges.end(), edge_less);
        std::sort(pending.taskTargets.begin(), pending.taskTargets.end(), task_less);
        pending.resources.erase(std::unique(pending.resources.begin(),
                                            pending.resources.end(),
                                            [](const auto& left, const auto& right) {
                                                return resource_natural(left)
                                                       == resource_natural(right);
                                            }),
                                pending.resources.end());
        pending.eventKeys.erase(std::unique(pending.eventKeys.begin(),
                                            pending.eventKeys.end(),
                                            [](const auto& left, const auto& right) {
                                                return event_key_natural(left)
                                                       == event_key_natural(right);
                                            }),
                                pending.eventKeys.end());
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
