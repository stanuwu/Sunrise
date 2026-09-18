#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

namespace format = state::activity_sdk::format;
namespace squad = squad_inventory;
namespace topology = topology_inventory;

/** A deferred authored-scene ID fits this storage including its terminator. */
inline constexpr std::size_t kTextCapacity = 256U;

/** One owned string retained until the final pack string table is linked. */
struct Text final {
    std::array<char, kTextCapacity> value{};
    std::uint16_t length{};
};

/** One exact format-v12 type-43 package resource before its ID is linked. */
struct Resource final {
    Text id{};
    std::uint32_t slotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t resourceFieldOffset{};
    std::uint32_t resourceTag{};
    std::uint32_t resourceClass{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One exact format-v12 same-object scene-to-squad edge before its ID is linked. */
struct SquadEdge final {
    Text id{};
    std::uint32_t sceneSlotIndex{};
    std::uint32_t squadSlotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t referenceFieldOffset{};
    std::uint32_t targetObjectKey{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One exact type-38 to type-3 authored target before its ID is linked. */
struct TaskTarget final {
    Text id{};
    std::uint32_t taskSlotIndex{};
    std::uint32_t objectiveSlotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t referenceFieldOffset{};
    std::uint32_t targetObjectKey{};
    std::uint32_t bitIndex{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One type-53 cue read from the list its slot's descriptor references. */
struct DialogueCue final {
    Text id{};
    std::uint32_t slotIndex{};
    std::uint32_t cueIndex{};
    std::uint32_t listTag{};
    std::uint32_t definitionHash{};
    float authoredWindowSeconds{};
    std::uint32_t lineCount{};
    std::uint32_t flags{};
};

/** One localized take of one line of a type-53 cue. */
struct DialogueCueText final {
    Text id{};
    Text text{};
    std::uint32_t slotIndex{};
    std::uint32_t cueIndex{};
    std::uint32_t definitionHash{};
    std::uint32_t containerTag{};
    std::uint32_t stringHash{};
    std::uint32_t lineIndex{};
    std::uint32_t takeIndex{};
    std::uint32_t audioTag{};
    std::uint32_t durationMs{};
};

/** One exact type-68 HUD element and its two authored localized fields. */
struct DirectiveElement final {
    Text id{};
    Text title{};
    Text description{};
    std::uint32_t slotIndex{};
    std::uint32_t nameHash{};
    std::int32_t elementIndex{};
    std::uint32_t elementCount{};
    std::uint32_t titleContainerTag{};
    std::uint32_t titleStringHash{};
    std::uint32_t descriptionContainerTag{};
    std::uint32_t descriptionStringHash{};
};

/** Scene-owned descriptor facts retain a complete source universe per selected object. */
struct Facts final {
    std::vector<squad::DescriptorFact> descriptors{};
    std::vector<squad::SlotSchemaFact> slotSchemas{};
    std::uint64_t partialChainCount{};
    bool complete{};
};

/** Canonical section-19 and section-22 rows with unresolved string references. */
struct Snapshot final {
    std::vector<Resource> resources{};
    std::vector<SquadEdge> squadEdges{};
    std::vector<TaskTarget> taskTargets{};
    std::vector<DialogueCue> dialogueCues{};
    std::vector<DialogueCueText> dialogueCueTexts{};
    std::vector<format::CombatObjectiveGroup> combatObjectiveGroups{};
    std::vector<DirectiveElement> directiveElements{};
    bool complete{};
};

/** Reads the exact scene-owning descriptor universe and proves parent-count closure. */
[[nodiscard]] bool collect_facts(const topology::Snapshot& topology,
                                 squad::TagReader reader,
                                 void* readerContext,
                                 std::span<const squad::SlotSchemaFact> slotSchemas,
                                 Facts& output) noexcept;

/** Reuses the complete descriptor universe already read for squad linking. */
[[nodiscard]] bool derive_facts(const topology::Snapshot& topology,
                                const squad::Facts& source,
                                Facts& output) noexcept;

/** Validates every structural ID, scalar domain, slot join, flag, and row order. */
[[nodiscard]] bool
validate(const topology::Snapshot& topology, const Facts& facts, const Snapshot& snapshot) noexcept;

/** Builds both authored-scene sections from complete topology and package facts. */
[[nodiscard]] bool build(const topology::Snapshot& topology,
                         const Facts& facts,
                         squad::TagReader reader,
                         void* readerContext,
                         Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
