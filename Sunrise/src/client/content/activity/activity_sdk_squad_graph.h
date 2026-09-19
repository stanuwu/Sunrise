#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "activity_sdk_squad_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {

/** Exact status of one authored spawner's unique source descriptor join. */
enum class SourceDescriptorStatus : std::uint8_t {
    exact,
    missing,
    ambiguous,
};

/** Exact result of decoding and resolving one authored spawner object-slot reference. */
enum class ReferenceResolutionStatus : std::uint8_t {
    exact,
    invalidEncoding,
    sourceDescriptorMissing,
    sourceDescriptorAmbiguous,
    targetMissing,
    targetDescriptorMismatch,
    targetAmbiguous,
};

/** Exact cardinality of one point identity in one rule config context's scenario. */
enum class PointContextStatus : std::uint8_t {
    unresolved,
    exact,
    ambiguous,
};

/** One normalized authored-spawner definition before scenario ownership is projected. */
struct GraphSpawner final {
    std::string id{};
    std::uint32_t configTag{};
    std::uint64_t rawReference98{};
    std::uint64_t rawReferenceA0{};
    std::uint64_t primaryComponentOffset{};
    std::uint64_t secondaryComponentOffset{};
    std::uint32_t primaryComponentClass{};
    std::uint32_t secondaryComponentClass{};
    format::Range members{};
    format::Range sourceDescriptorCandidates{};
    format::Range references{};
    std::uint32_t sourceDescriptorRow{format::kAbsentIndex};
    SourceDescriptorStatus sourceDescriptorStatus{SourceDescriptorStatus::missing};
    bool complete{};
    /** The rule row standing for the spawner's own point set, when it carries one. */
    bool hasInlinePointSet{};
    /** Both native references are absent and canonical parsing proved no inline fallback. */
    bool requiresSelectedRule{};
    std::uint32_t inlineRuleRow{format::kAbsentIndex};
};

/** One normalized authored member with six exact candidate-lane ranges. */
struct GraphMember final {
    std::string id{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t memberOrdinal{};
    std::uint32_t memberKey{};
    std::uint32_t reservedU32{};
    std::array<format::Range, format::kSquadCandidateCountLaneCount> candidateLanes{};
};

/** One complete stable candidate row, including null and sentinel-valued placements. */
struct GraphCandidate final {
    std::string id{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t memberRow{format::kAbsentIndex};
    std::uint32_t memberOrdinal{};
    std::uint32_t lane{};
    std::uint32_t candidateOrdinal{};
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    CandidateState state{CandidateState::nullPlacement};
    std::uint64_t candidateDescriptorOffset{};
    std::int64_t placementRelative{};
    std::uint64_t placementOffset{};
    std::array<std::byte, 16> candidateTail{};
    std::uint32_t placedEntryClass{format::kAbsentIndex};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::array<std::uint32_t, 3> positionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::uint64_t placedEntryIdentity{};
};

/** One normalized authored spawn-rule definition. */
struct GraphRule final {
    std::string id{};
    std::uint32_t configTag{};
    std::uint64_t primaryComponentOffset{};
    std::uint64_t secondaryComponentOffset{};
    std::uint32_t primaryComponentClass{};
    std::uint32_t secondaryComponentClass{};
    format::Range points{};
    bool complete{};
    /** An inline rule belongs to one spawner and shares its config tag. */
    bool inlineForm{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
};

/** One complete 0x48 authored point row. */
struct GraphPoint final {
    std::string id{};
    std::uint32_t ruleRow{format::kAbsentIndex};
    std::uint32_t pointOrdinal{};
    std::uint64_t placedEntryIdentity{};
    std::uint64_t rowOffset{};
    std::array<std::byte, 64> rawTail{};
    format::Range contexts{};
};

/** One package descriptor retained as authored graph provenance. */
struct GraphDescriptor final {
    std::string id{};
    std::uint32_t configTag{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t slotIndex{format::kAbsentIndex};
    std::uint32_t descriptorOffset{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    bool complete{};
};

/** One raw slot-schema input retained without presenting it as a replacement schema family. */
struct GraphSlotSchemaProvenance final {
    std::uint32_t slotIndex{format::kAbsentIndex};
    std::uint32_t componentClass{format::kAbsentIndex};
    std::uint32_t senseSchema{format::kAbsentIndex};
    std::uint32_t authSchema{format::kAbsentIndex};
    bool exact{};
};

/** One reached actor definition tag retained as provenance for the actor_classes family. */
struct GraphActorDefinitionProvenance final {
    std::uint32_t definitionTag{format::kAbsentIndex};
};

/** One source-descriptor candidate owned by an authored spawner. */
struct GraphSourceDescriptorCandidate final {
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t descriptorRow{format::kAbsentIndex};
};

/** One candidate descriptor inspected while resolving a raw spawner reference. */
struct GraphReferenceDescriptor final {
    std::uint32_t referenceRow{format::kAbsentIndex};
    std::uint32_t descriptorRow{format::kAbsentIndex};
    bool authoritative{};
    bool resolvedTarget{};
};

/** One decoded raw 0x98 or 0xA0 spawner reference and its complete resolution evidence. */
struct GraphReference final {
    std::uint32_t spawnerRow{format::kAbsentIndex};
    /** Exact occurrence-scoped source when one config is reused by separate scenarios. */
    std::uint32_t sourceDescriptorRow{format::kAbsentIndex};
    std::uint32_t referenceOrdinal{};
    std::uint64_t rawReference{};
    std::uint32_t targetObjectKey{};
    std::uint32_t targetSlotType{};
    std::uint32_t targetSlotIndex{};
    format::Range candidateDescriptors{};
    std::uint32_t resolvedRuleRow{format::kAbsentIndex};
    std::uint32_t resolvedObjectRow{format::kAbsentIndex};
    std::uint32_t resolvedSlotRow{format::kAbsentIndex};
    ReferenceResolutionStatus status{ReferenceResolutionStatus::targetMissing};
    bool encodingValid{};
};

/** One normalized authored-spawner to authored-rule definition edge. */
struct GraphEdge final {
    std::string id{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t ruleRow{format::kAbsentIndex};
    std::uint32_t sourceDescriptorRow{format::kAbsentIndex};
    std::uint32_t targetDescriptorRow{format::kAbsentIndex};
    std::uint32_t targetObjectRow{format::kAbsentIndex};
    std::uint32_t targetSlotRow{format::kAbsentIndex};
    std::uint32_t referenceMask{};
    format::Range targetDescriptors{};
    format::Range scenarioContexts{};
    bool associationExact{};
    bool sameObject{};
};

/** One descriptor in an edge's exact logical target group. */
struct GraphEdgeTargetDescriptor final {
    std::uint32_t edgeRow{format::kAbsentIndex};
    std::uint32_t descriptorRow{format::kAbsentIndex};
};

/** One scenario-owned config context. */
struct GraphConfigContext final {
    std::string id{};
    std::uint32_t globalRow{format::kAbsentIndex};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t configTag{};
    std::uint32_t occurrenceIndex{format::kAbsentIndex};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t spawnerRow{format::kAbsentIndex};
    std::uint32_t ruleRow{format::kAbsentIndex};
    bool complete{};
};

/** One scenario-owned authored placement context. */
struct GraphPlacementContext final {
    std::string id{};
    std::uint32_t globalRow{format::kAbsentIndex};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t occurrenceIndex{format::kAbsentIndex};
    /** Set for a container or descriptor-embedded row, which no activity occurrence owns. */
    bool external{};
    std::uint32_t objectListTag{};
    std::uint32_t placementOrdinal{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    std::uint64_t sourceOffset{};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::int64_t auxiliaryRelative{};
    bool complete{};
};

/** One point/config-context cardinality summary, including zero and ambiguous matches. */
struct GraphPointContext final {
    std::uint32_t globalRow{format::kAbsentIndex};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t pointRow{format::kAbsentIndex};
    std::uint32_t configContextRow{format::kAbsentIndex};
    format::Range matches{};
    PointContextStatus status{PointContextStatus::unresolved};
};

/** One exact same-scenario point-to-placement identity match. */
struct GraphPointPlacementMatch final {
    std::uint32_t globalRow{format::kAbsentIndex};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t pointContextRow{format::kAbsentIndex};
    std::uint32_t pointRow{format::kAbsentIndex};
    std::uint32_t configContextRow{format::kAbsentIndex};
    std::uint32_t placementContextRow{format::kAbsentIndex};
    std::uint64_t placedEntryIdentity{};
    bool sameOccurrence{};
};

/** One scenario membership owned by a global spawner-rule edge. */
struct GraphEdgeContext final {
    std::uint32_t globalRow{format::kAbsentIndex};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t edgeRow{format::kAbsentIndex};
};

/** Lossless normalized authored-squad graph split into global definitions and scenario contexts. */
struct GraphSnapshot final {
    std::vector<GraphSpawner> spawners{};
    std::vector<GraphMember> members{};
    std::vector<GraphCandidate> candidates{};
    std::vector<GraphRule> rules{};
    std::vector<GraphPoint> points{};
    std::vector<GraphDescriptor> descriptors{};
    std::vector<GraphSlotSchemaProvenance> slotSchemas{};
    std::vector<GraphActorDefinitionProvenance> actorDefinitions{};
    std::vector<GraphSourceDescriptorCandidate> sourceDescriptorCandidates{};
    std::vector<GraphReference> references{};
    std::vector<GraphReferenceDescriptor> referenceDescriptors{};
    std::vector<GraphEdge> edges{};
    std::vector<GraphEdgeTargetDescriptor> edgeTargetDescriptors{};
    std::vector<GraphConfigContext> configContexts{};
    std::vector<GraphPlacementContext> placementContexts{};
    std::vector<GraphPointContext> pointContexts{};
    std::vector<GraphPointPlacementMatch> pointPlacementMatches{};
    std::vector<GraphEdgeContext> edgeContexts{};
    bool ready{};
};

/** Builds one deterministic graph without publishing, serializing, or creating runnable squads. */
[[nodiscard]] bool build_graph(const topology_inventory::Snapshot& topology,
                               const Facts& facts,
                               GraphSnapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
