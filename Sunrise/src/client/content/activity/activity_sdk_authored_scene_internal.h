#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "activity_sdk_authored_scene_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

/** One exact slot schema indexed by the canonical global slot row. */
using SchemaIndex = std::unordered_map<std::uint32_t, const squad::SlotSchemaFact*>;

/** Formats one structural ID without truncation. */
template <typename... Values>
[[nodiscard]] inline bool format_text(Text& output, const char* spec, Values... values) noexcept {
    output = {};
    const int written = std::snprintf(output.value.data(), output.value.size(), spec, values...);
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** @return True when all fixed string storage follows the deferred-text contract. */
[[nodiscard]] inline bool valid_text(const Text& text) noexcept {
    if (text.length >= text.value.size() || text.value[text.length] != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < text.length; ++index) {
        if (text.value[index] == '\0') {
            return false;
        }
    }
    return std::all_of(text.value.begin() + text.length + 1U, text.value.end(), [](char value) {
        return value == '\0';
    });
}

/** Returns one deferred string after validating its fixed storage. */
[[nodiscard]] inline bool text_view(const Text& text, std::string_view& output) noexcept {
    output = {};
    if (!valid_text(text)) {
        return false;
    }
    output = std::string_view(text.value.data(), text.length);
    return true;
}

/** Tests the exact type-43 descriptor shape before package projection. */
[[nodiscard]] inline bool is_scene_descriptor(const topology::Snapshot& topology,
                                              const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kAuthoredSceneSlotType
           && descriptor.componentClass == format::kAuthoredSceneComponentClass
           && descriptor.senseSchema == format::kAuthoredSceneSenseSchema
           && descriptor.authSchema == format::kAuthoredSceneAuthSchema;
}

/** Tests the exact type-42 descriptor shape before package projection. */
[[nodiscard]] inline bool
is_performance_descriptor(const topology::Snapshot& topology,
                          const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kPerformanceSlotType
           && descriptor.componentClass == format::kPerformanceComponentClass
           && descriptor.senseSchema == format::kAbsentIndex
           && descriptor.authSchema == format::kPerformanceAuthSchema;
}

/** Tests the exact type-38 descriptor shape before package projection. */
[[nodiscard]] inline bool is_task_descriptor(const topology::Snapshot& topology,
                                             const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kTaskSlotType
           && descriptor.componentClass == format::kTaskComponentClass
           && descriptor.senseSchema == format::kAbsentIndex
           && descriptor.authSchema == format::kTaskAuthSchema;
}

/** Sort key matching the final section-19 pack order. */
[[nodiscard]] inline auto resource_natural(const Resource& row) noexcept {
    return std::tie(row.slotIndex, row.configTag, row.descriptorOffset);
}

/** Sort key matching the final section-22 pack order. */
[[nodiscard]] inline auto edge_natural(const SquadEdge& row) noexcept {
    return std::tie(row.sceneSlotIndex, row.configTag, row.descriptorOffset, row.squadSlotIndex);
}

/** Sort key matching the final task-target pack order. */
[[nodiscard]] inline auto task_natural(const TaskTarget& row) noexcept {
    return std::tie(row.taskSlotIndex, row.configTag, row.descriptorOffset);
}

/** Compares resource rows in final pack order. */
[[nodiscard]] inline bool resource_less(const Resource& left, const Resource& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.slotIndex, left.configTag, left.descriptorOffset, left.resourceTag, leftId)
           < std::tie(right.slotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.resourceTag,
                      rightId);
}

/** Compares edge rows in final pack order. */
[[nodiscard]] inline bool edge_less(const SquadEdge& left, const SquadEdge& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.sceneSlotIndex,
                    left.configTag,
                    left.descriptorOffset,
                    left.squadSlotIndex,
                    leftId)
           < std::tie(right.sceneSlotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.squadSlotIndex,
                      rightId);
}

/** Compares task-target rows in final pack order. */
[[nodiscard]] inline bool task_less(const TaskTarget& left, const TaskTarget& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.taskSlotIndex,
                    left.configTag,
                    left.descriptorOffset,
                    left.objectiveSlotIndex,
                    left.bitIndex,
                    leftId)
           < std::tie(right.taskSlotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.objectiveSlotIndex,
                      right.bitIndex,
                      rightId);
}

/** Builds and validates the unique global schema lookup. */
[[nodiscard]] bool
schema_index(const topology::Snapshot& topology, const Facts& facts, SchemaIndex& output);

/** Checks the topology fields consumed by this bounded projection. */
[[nodiscard]] bool valid_topology(const topology::Snapshot& topology) noexcept;

/** Tests one exact final slot shape supplied by the separate schema join. */
[[nodiscard]] bool slot_shape(const topology::Snapshot& topology,
                              const SchemaIndex& schemas,
                              std::uint32_t slotIndex,
                              std::uint32_t slotType,
                              std::uint32_t componentClass,
                              std::uint32_t senseSchema,
                              std::uint32_t authSchema) noexcept;

/** Formats one resource ID from its exact descriptor tuple. */
[[nodiscard]] bool resource_id(const topology::Snapshot& topology,
                               const squad::DescriptorFact& descriptor,
                               Text& output) noexcept;

/** Formats one scene-to-squad edge ID from its descriptor tuple and the squad slot it names. */
[[nodiscard]] bool edge_id(const topology::Snapshot& topology,
                           const squad::DescriptorFact& descriptor,
                           std::uint32_t squadSlotRow,
                           Text& output) noexcept;

/** Formats one task-to-objective target ID from its exact descriptor tuple. */
[[nodiscard]] bool task_target_id(const topology::Snapshot& topology,
                                  const squad::DescriptorFact& descriptor,
                                  Text& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
