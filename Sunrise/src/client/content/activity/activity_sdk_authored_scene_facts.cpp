#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808062.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808063.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80807d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808095.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "activity_sdk_authored_scene_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {
namespace {

namespace tables = middleware::content::packages::tables;
namespace generated = middleware::content::packages::schema::generated;

/** One object-local descriptor walk and its replaceable package-read storage. */
struct DescriptorWalk final {
    const topology::Snapshot* topology{};
    std::uint32_t objectIndex{};
    squad::TagReader reader{};
    void* readerContext{};
    std::vector<std::byte> readStorage{};
    std::vector<squad::DescriptorFact> descriptors{};
    bool valid{true};
};

/** Checks the topology lanes consumed before the first package read. */
[[nodiscard]] bool valid_topology(const topology::Snapshot& topology) noexcept {
    if (!topology.ready || topology.objects.empty() || topology.slots.empty()
        || topology.objects.size() >= format::kAbsentIndex
        || topology.slots.size() >= format::kAbsentIndex) {
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
        for (std::uint32_t slotIndex = object.firstSlot;
             slotIndex < object.firstSlot + object.slotCount;
             ++slotIndex) {
            const topology::Slot& slot = topology.slots[slotIndex];
            if (slot.objectIndex != objectIndex || slot.slotIndex != slotIndex - object.firstSlot) {
                return false;
            }
        }
        nextSlot += object.slotCount;
    }
    return nextSlot == topology.slots.size();
}

/** Indexes one complete slot-schema join and marks objects projected by this inventory. */
[[nodiscard]] bool index_schemas(const topology::Snapshot& topology,
                                 std::span<const squad::SlotSchemaFact> schemas,
                                 std::vector<const squad::SlotSchemaFact*>& bySlot,
                                 std::vector<bool>& projectedOwners) {
    bySlot.clear();
    projectedOwners.clear();
    if (schemas.size() != topology.slots.size()) {
        return false;
    }
    try {
        bySlot.resize(topology.slots.size());
        projectedOwners.resize(topology.objects.size());
        for (const squad::SlotSchemaFact& schema : schemas) {
            if (schema.slotIndex >= topology.slots.size() || bySlot[schema.slotIndex] != nullptr) {
                return false;
            }
            bySlot[schema.slotIndex] = &schema;
            const topology::Slot& slot = topology.slots[schema.slotIndex];
            if (slot.objectIndex >= topology.objects.size()) {
                return false;
            }
            if (slot.slotType == format::kAuthoredSceneSlotType && schema.exact
                && schema.componentClass == generated::AuthoredSceneComponentType43::kClass
                && schema.senseSchema == generated::C8080626a::kClass
                && schema.authSchema == generated::C8080626b::kClass) {
                projectedOwners[slot.objectIndex] = true;
            }
            if (slot.slotType == format::kTaskSlotType && schema.exact
                && schema.componentClass == generated::TaskComponent::kClass
                && schema.senseSchema == format::kAbsentIndex
                && schema.authSchema == generated::C80807d89::kClass) {
                projectedOwners[slot.objectIndex] = true;
            }
            // A performance sensor names a squad in its own object, so that object must be
            // walked even when it holds no scene and no task.
            if (slot.slotType == format::kPerformanceSlotType && schema.exact
                && schema.componentClass == generated::PerformanceSensorComponent::kClass
                && schema.senseSchema == format::kAbsentIndex
                && schema.authSchema == generated::C80809586::kClass) {
                projectedOwners[slot.objectIndex] = true;
            }
        }
        return std::all_of(
            bySlot.begin(), bySlot.end(), [](const auto* schema) { return schema != nullptr; });
    } catch (...) {
        bySlot.clear();
        projectedOwners.clear();
        return false;
    }
}

/** Reads one placed-chain tag while keeping its bytes live for the current visit. */
[[nodiscard]] bool descriptor_chain_read(void* opaque,
                                         std::uint32_t tag,
                                         std::span<const std::byte>& blob,
                                         std::uint32_t& classId) noexcept {
    blob = {};
    classId = 0;
    if (opaque == nullptr) {
        return false;
    }
    auto& walk = *static_cast<DescriptorWalk*>(opaque);
    if (walk.reader == nullptr
        || !walk.reader(walk.readerContext, tag, walk.readStorage, classId)) {
        return false;
    }
    blob = walk.readStorage;
    return true;
}

/** Formats the canonical descriptor identity for one matched local slot. */
[[nodiscard]] bool descriptor_id(const topology::Object& object,
                                 const topology::Slot& slot,
                                 const tables::SlotDescriptor& source,
                                 std::string& output) {
    output.clear();
    std::array<char, 80> buffer{};
    const int written = std::snprintf(buffer.data(),
                                      buffer.size(),
                                      "descriptor/%08x/%08x/%08x/%04x/%04x",
                                      static_cast<unsigned>(source.configTag),
                                      static_cast<unsigned>(object.objectTag),
                                      static_cast<unsigned>(source.descriptorOffset),
                                      static_cast<unsigned>(slot.slotIndex),
                                      static_cast<unsigned>(slot.slotType));
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return false;
    }
    try {
        output.assign(buffer.data(), static_cast<std::size_t>(written));
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Retains one descriptor matched to its exact object-local slot. */
[[nodiscard]] bool descriptor_visit(void* opaque, const tables::SlotDescriptor& source) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& walk = *static_cast<DescriptorWalk*>(opaque);
    if (walk.topology == nullptr || walk.objectIndex >= walk.topology->objects.size()) {
        return false;
    }
    if (source.slotIndex > static_cast<std::uint16_t>((std::numeric_limits<std::int16_t>::max)())) {
        return true;
    }
    const topology::Object& object = walk.topology->objects[walk.objectIndex];
    const auto first = walk.topology->slots.begin() + object.firstSlot;
    const auto last = first + object.slotCount;
    const auto slot = std::find_if(first, last, [&source](const topology::Slot& row) {
        return row.slotType == source.slotType && row.slotIndex == source.slotIndex;
    });
    if (slot == last) {
        return true;
    }
    squad::DescriptorFact row{};
    row.configTag = source.configTag;
    row.objectIndex = walk.objectIndex;
    row.slotIndex = static_cast<std::uint32_t>(slot - walk.topology->slots.begin());
    row.descriptorOffset = source.descriptorOffset;
    row.componentClass = source.componentClass;
    row.senseSchema = source.senseSchema;
    row.authSchema = source.authSchema;
    row.complete = true;
    if (!descriptor_id(object, *slot, source, row.id)) {
        walk.valid = false;
        return false;
    }
    try {
        walk.descriptors.push_back(std::move(row));
        return true;
    } catch (...) {
        walk.valid = false;
        return false;
    }
}

/** Parses every descriptor in one reached placed-config row. */
[[nodiscard]] bool
descriptor_config(void* opaque, std::uint32_t configTag, std::span<const std::byte> blob) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& walk = *static_cast<DescriptorWalk*>(opaque);
    if (walk.topology == nullptr || walk.objectIndex >= walk.topology->objects.size()) {
        return false;
    }
    return tables::visit_slot_descriptors(blob,
                                          configTag,
                                          walk.topology->objects[walk.objectIndex].objectKey,
                                          &descriptor_visit,
                                          &walk);
}

/** Direct authored-placement branches contain no slot descriptor rows. */
[[nodiscard]] bool descriptor_bare_target(void*, std::uint32_t, std::uint32_t) noexcept {
    return true;
}

/** Compares repeated paths to the same descriptor identity. */
[[nodiscard]] bool same_descriptor(const squad::DescriptorFact& left,
                                   const squad::DescriptorFact& right) noexcept {
    return left.id == right.id && left.configTag == right.configTag
           && left.objectIndex == right.objectIndex && left.slotIndex == right.slotIndex
           && left.descriptorOffset == right.descriptorOffset
           && left.componentClass == right.componentClass && left.senseSchema == right.senseSchema
           && left.authSchema == right.authSchema && left.complete == right.complete;
}

/** Validates the source object's package identity and exact local slot table. */
[[nodiscard]] bool object_header(const topology::Snapshot& topology,
                                 std::uint32_t objectIndex,
                                 std::span<const std::byte> bytes) noexcept {
    if (objectIndex >= topology.objects.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[objectIndex];
    std::uint32_t objectKey = 0;
    tables::Array slots{};
    if (!tables::object_key(bytes, objectKey) || objectKey != object.objectKey
        || !tables::object_slots(bytes, slots) || slots.count != object.slotCount) {
        return false;
    }
    for (std::uint64_t ordinal = 0; ordinal < slots.count; ++ordinal) {
        tables::Slot source{};
        const topology::Slot& expected = topology.slots[object.firstSlot + ordinal];
        if (!tables::object_slot_at(bytes, slots, ordinal, source)
            || expected.objectIndex != objectIndex || expected.slotIndex != ordinal
            || expected.slotType != source.type || expected.nameHash != source.nameHash) {
            return false;
        }
    }
    return true;
}

/** Walks one exact scene-owning object and proves its descriptor-count closure. */
[[nodiscard]] bool collect_object(const topology::Snapshot& topology,
                                  std::uint32_t objectIndex,
                                  squad::TagReader reader,
                                  void* readerContext,
                                  Facts& output) {
    if (objectIndex >= topology.objects.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[objectIndex];
    if (!object.descriptorEvidenceComplete || object.descriptorCount == format::kAbsentIndex) {
        return false;
    }
    std::vector<std::byte> objectBytes{};
    std::uint32_t classId = 0;
    tables::Array bubbles{};
    if (!reader(readerContext, object.objectTag, objectBytes, classId)
        || classId != tables::kObjectClass || !object_header(topology, objectIndex, objectBytes)
        || !tables::object_bubbles(objectBytes, bubbles)) {
        return false;
    }
    DescriptorWalk walk{&topology, objectIndex, reader, readerContext};
    walk.descriptors.reserve(object.descriptorCount);
    for (std::uint64_t subblock = 0; subblock < bubbles.count; ++subblock) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(objectBytes, bubbles, subblock, bubble)) {
            return false;
        }
        for (std::uint64_t leaf = 0; leaf < bubble.handleCount; ++leaf) {
            std::uint32_t handle = 0;
            tables::PlacedChainObservation observation{};
            if (!tables::object_placed_handle_at(objectBytes, bubble, leaf, handle)) {
                return false;
            }
            if (!tables::observe_placed_chain(handle,
                                              &descriptor_chain_read,
                                              &walk,
                                              &descriptor_config,
                                              &walk,
                                              &descriptor_bare_target,
                                              &walk,
                                              observation)
                || !observation.complete) {
                if (output.partialChainCount == (std::numeric_limits<std::uint64_t>::max)()) {
                    return false;
                }
                ++output.partialChainCount;
            }
            if (!walk.valid) {
                return false;
            }
        }
    }
    std::sort(walk.descriptors.begin(),
              walk.descriptors.end(),
              [](const auto& first, const auto& second) { return first.id < second.id; });
    std::vector<squad::DescriptorFact> unique{};
    unique.reserve(walk.descriptors.size());
    for (squad::DescriptorFact& descriptor : walk.descriptors) {
        if (!unique.empty() && unique.back().id == descriptor.id) {
            if (!same_descriptor(unique.back(), descriptor)) {
                return false;
            }
            continue;
        }
        unique.push_back(std::move(descriptor));
    }
    if (unique.size() != object.descriptorCount) {
        return false;
    }
    for (squad::DescriptorFact& descriptor : unique) {
        output.descriptors.push_back(std::move(descriptor));
    }
    return true;
}

} // namespace

/** Reads the exact scene-owning descriptor universe and proves parent-count closure. */
bool collect_facts(const topology::Snapshot& topology,
                   squad::TagReader reader,
                   void* readerContext,
                   std::span<const squad::SlotSchemaFact> slotSchemas,
                   Facts& output) noexcept {
    output = {};
    if (reader == nullptr || !valid_topology(topology)) {
        return false;
    }
    try {
        std::vector<const squad::SlotSchemaFact*> schemasBySlot{};
        std::vector<bool> projectedOwners{};
        if (!index_schemas(topology, slotSchemas, schemasBySlot, projectedOwners)) {
            return false;
        }
        Facts pending{};
        pending.slotSchemas.assign(slotSchemas.begin(), slotSchemas.end());
        for (std::uint32_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
            if (projectedOwners[objectIndex]
                && !collect_object(topology, objectIndex, reader, readerContext, pending)) {
                return false;
            }
        }
        std::sort(pending.descriptors.begin(),
                  pending.descriptors.end(),
                  [](const auto& first, const auto& second) { return first.id < second.id; });
        if (std::adjacent_find(
                pending.descriptors.begin(),
                pending.descriptors.end(),
                [](const auto& first, const auto& second) { return first.id == second.id; })
            != pending.descriptors.end()) {
            return false;
        }
        pending.complete = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

/** Derives the authored scene facts from a squad pass. @return False on an incomplete source. */
bool derive_facts(const topology::Snapshot& topology,
                  const squad::Facts& source,
                  Facts& output) noexcept {
    output = {};
    if (!source.complete || !valid_topology(topology)
        || source.slotSchemas.size() != topology.slots.size()) {
        return false;
    }
    try {
        std::vector<const squad::SlotSchemaFact*> schemasBySlot{};
        std::vector<bool> projectedOwners{};
        if (!index_schemas(topology, source.slotSchemas, schemasBySlot, projectedOwners)) {
            return false;
        }
        Facts pending{};
        pending.slotSchemas = source.slotSchemas;
        pending.descriptors.reserve(source.descriptors.size());
        for (const squad::DescriptorFact& descriptor : source.descriptors) {
            if (descriptor.objectIndex >= projectedOwners.size()) {
                return false;
            }
            if (projectedOwners[descriptor.objectIndex]) {
                pending.descriptors.push_back(descriptor);
            }
        }
        pending.complete = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
