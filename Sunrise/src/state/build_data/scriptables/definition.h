#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sunrise::state::build_data::scriptables {

/** Fixed name storage on disk and in memory; the last byte holds the terminator. */
inline constexpr std::size_t kScenarioNameCapacity = 64;
inline constexpr std::size_t kNameCapacity = 128;
/** Raw package-inline name evidence keeps complete strings through this byte count. */
inline constexpr std::size_t kInlineNameMaximumBytes = 4096;
/** Authored placed-object chains reject a ninth child edge, matching the package reader. */
inline constexpr std::size_t kPlacedBranchPathCapacity = 9;
inline constexpr std::uint32_t kNoRow = 0xFFFFFFFFU;

enum class BuildStatus : std::uint8_t {
    idle,
    queued,
    building,
    ready,
    failed,
};

/** Package-derived domains present in one ready snapshot. */
enum class BuildCoverage : std::uint8_t {
    none,
    full,
};

/** Result of one structural-family extraction pass. */
enum class FamilyCoverageStatus : std::uint8_t {
    unassessed,
    complete,
    notApplicable,
    preservedUnresolved,
    incomplete,
};

/** Structural loss causes may be combined for one family. */
enum FamilyCoverageLoss : std::uint8_t {
    kFamilyCoverageLossNone = 0,
    kFamilyCoverageLossUnread = 1U << 0U,
    kFamilyCoverageLossDropped = 1U << 1U,
    kFamilyCoverageLossPartial = 1U << 2U,
};

/** Fixed structural families covered by one scenario shard. */
enum class StructuralFamily : std::uint8_t {
    scenarioTopology,
    objectGraph,
    typedReferences,
    authoredPlacements,
    containerPlacements,
    embeddedPlacements,
    type23Placements,
    staticSpatial,
    triggerVolumes,
    names,
    count,
};

/** Coverage carries one result per structural family, so the count fixes that array. */
inline constexpr std::size_t kStructuralFamilyCount =
    static_cast<std::size_t>(StructuralFamily::count);
static_assert(kStructuralFamilyCount == 10);

/** One family result plus exact structural loss categories. */
struct FamilyCoverageDiagnostic final {
    FamilyCoverageStatus status{FamilyCoverageStatus::unassessed};
    std::uint8_t lossMask{kFamilyCoverageLossNone};

    [[nodiscard]] bool operator==(const FamilyCoverageDiagnostic&) const noexcept = default;
};

/** Authenticated extraction results for every fixed structural family. */
struct CoverageDiagnostics final {
    std::array<FamilyCoverageDiagnostic, kStructuralFamilyCount> families{};

    [[nodiscard]] bool operator==(const CoverageDiagnostics&) const noexcept = default;
};

enum class NameProvenance : std::uint8_t {
    unresolved,
    packageInline,
    packagePath,
    packageIdentifierCandidate,
};

enum class GroupSafety : std::uint8_t {
    notApplicable,
    destinationSafe,
    bubbleSafe,
    stateOnly,
    incomplete,
    ambiguous,
};

enum class ReferenceJoin : std::uint8_t {
    unresolved,
    exact,
    ambiguous,
};

enum class SpatialContextJoin : std::uint8_t {
    unresolved,
    packageObjectState,
    packageStemBubble,
};

/** One scenario bubble, before any registry objects are flattened. */
struct Bubble final {
    std::uint32_t nameHash{};
    std::uint32_t firstState{};
    std::uint32_t stateCount{};
    std::uint32_t nameRow{kNoRow};
    std::uint32_t index{};
    bool isPublic{};
};

/** One authored slice-set state. `resolved` is false when its entry or registry did not read. */
struct State final {
    std::uint32_t stateHash{};
    /** Exact little-endian state word at scenario-record offset 12. */
    std::uint32_t rawU32At12{};
    std::uint32_t entryTag{};
    std::uint32_t registryTag{};
    std::uint32_t sliceSetIndex{};
    std::uint32_t mapBubbleIndex{};
    std::uint32_t bubbleRow{};
    std::uint32_t nameRow{kNoRow};
    std::uint32_t entryNameRow{kNoRow};
    std::uint32_t registryNameRow{kNoRow};
    std::uint32_t index{};
    bool enabled{};
    bool resolved{};
};

/** One exact object placement in one state registry array. */
struct Object final {
    std::uint32_t bubbleRow{};
    std::uint32_t stateRow{};
    std::uint32_t registryTag{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
    std::uint32_t firstSlot{};
    std::uint32_t slotCount{};
    std::uint32_t configCount{};
    std::uint32_t placedSubblockCount{};
    std::uint32_t placedLeafCount{};
    std::uint32_t placedHopCount{};
    std::uint32_t bareTargetCount{};
    std::uint32_t placedConfigOccurrenceCount{};
    /** Authored placements the game replicates. Zero with placements means client-built. */
    std::uint32_t replicatedPlacementCount{};
    std::uint32_t objectIndex{};
    std::uint32_t registryNameRow{kNoRow};
    std::uint32_t objectNameRow{kNoRow};
    std::uint16_t registryDescriptor{};
    GroupSafety safety{GroupSafety::notApplicable};
    /** True when the package walk completed; declared slots may still have zero descriptors. */
    bool complete{};
};

/** Exact validated shape of one path-specific placed-object chain node. */
enum class PlacedHopShape : std::uint8_t {
    config,
    redirect,
    descriptorRedirectArray,
    bareObjectList,
};

/** Whether a direct object-list target was fully retained. */
enum class PlacedBareTargetStatus : std::uint8_t {
    completeStructuralEdge,
    unreadableTarget,
    targetClassMismatch,
};

/** One exact per-bubble sub-block from an object definition. */
struct PlacedSubblock final {
    std::uint32_t objectRow{};
    std::uint32_t subblockOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t firstLeaf{};
    std::uint32_t leafCount{};
    std::uint64_t sourceOffset{};
    bool complete{};
};

/** One authored root handle and all path-specific rows reached from it. */
struct PlacedLeaf final {
    std::uint32_t objectRow{};
    std::uint32_t subblockRow{};
    std::uint32_t subblockOrdinal{};
    std::uint32_t leafOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t rootTag{};
    std::uint32_t firstHop{};
    std::uint32_t hopCount{};
    std::uint32_t firstConfigOccurrence{};
    std::uint32_t configOccurrenceCount{};
    std::uint32_t firstBareTarget{};
    std::uint32_t bareTargetCount{};
    std::uint64_t sourceOffset{};
    bool complete{};
};

/** One package tag retained once for each authored branch path that reaches it. */
struct PlacedHop final {
    std::uint32_t objectRow{};
    std::uint32_t subblockRow{};
    std::uint32_t leafRow{};
    std::uint32_t subblockOrdinal{};
    std::uint32_t leafOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::array<std::byte, 32> payloadSha256{};
    std::array<std::uint8_t, kPlacedBranchPathCapacity> branchPath{};
    std::uint32_t childCount{};
    std::uint32_t directTargetTag{};
    std::uint32_t configOccurrenceRow{kNoRow};
    std::uint32_t bareTargetRow{kNoRow};
    std::uint8_t branchPathCount{};
    std::uint8_t depth{};
    PlacedHopShape shape{PlacedHopShape::config};
    bool complete{};
};

/** One path-specific terminal config occurrence, before global config definition linking. */
struct PlacedConfigOccurrence final {
    std::uint32_t objectRow{};
    std::uint32_t subblockRow{};
    std::uint32_t leafRow{};
    std::uint32_t terminalHopRow{};
    std::uint32_t configTag{};
    std::int32_t declaredBubbleIndex{};
    std::array<std::uint8_t, kPlacedBranchPathCapacity> branchPath{};
    std::uint8_t branchPathCount{};
    bool complete{};
};

/** One direct structural edge from a placed handle to an authored object-list target. */
struct PlacedBareTarget final {
    std::uint32_t objectRow{};
    std::uint32_t subblockRow{};
    std::uint32_t leafRow{};
    std::uint32_t sourceHopRow{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t targetTag{};
    std::uint32_t expectedTargetClass{};
    std::uint32_t targetClass{kNoRow};
    std::uint64_t targetLogicalSize{};
    std::array<std::byte, 32> targetPayloadSha256{};
    PlacedBareTargetStatus status{PlacedBareTargetStatus::unreadableTarget};
};

/** One declared slot from an object's own slot table. */
struct Slot final {
    std::uint32_t objectRow{};
    std::uint32_t nameHash{};
    std::uint32_t nameRow{kNoRow};
    std::uint32_t firstDescriptor{};
    std::uint32_t descriptorCount{};
    std::uint16_t slotIndex{};
    std::uint16_t slotType{};
};

/** One descriptor proved by tag, marker, registry key, and exact object-slot identity. */
struct Descriptor final {
    std::uint32_t slotRow{};
    std::uint32_t configTag{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    std::uint32_t descriptorOffset{};
    std::uint32_t bubbleIndex{};
    std::uint32_t configNameRow{kNoRow};
    std::uint64_t placementIdentifier{};
    std::uint32_t placementLinkRow{kNoRow};
    std::uint32_t embeddedPlacementLinkRow{kNoRow};
    bool placementIdentifierRead{};
};

/** One exact type-4 descriptor linked to its retained native placement-array candidates. */
struct EmbeddedPlacementLink final {
    std::uint32_t descriptorRow{};
    std::uint32_t slotRow{kNoRow};
    std::uint32_t objectRow{kNoRow};
    std::uint32_t firstCandidate{};
    std::uint32_t candidateCount{};
    std::uint64_t declaredPlacementCount{};
    std::size_t arrayDataOffset{};
    bool complete{};
};

/** One validated native placement-array row retained under an exact type-4 descriptor link. */
struct EmbeddedPlacement final {
    std::uint32_t linkRow{};
    std::uint32_t entryIndex{};
    std::size_t sourceOffset{};
    std::uint32_t classListTag{};
    std::uint32_t classListNameRow{kNoRow};
    std::uint32_t nameHash{};
    std::uint64_t identifier{};
    std::int64_t auxiliaryRelative{};
    std::size_t auxiliaryOffset{};
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    float fourthLane{};
    std::uint8_t replicationByte{};
    std::uint8_t gameworldByte{};
    std::uint8_t objectType{};
    bool hasAuxiliary{};
    bool objectTypeRead{};
};

/** Completeness and exact capacity losses of the descriptor-embedded placement pass. */
struct EmbeddedPlacementDiagnostics final {
    std::uint64_t applicableDescriptors{};
    std::uint64_t emptyDescriptors{};
    std::uint64_t readPlacements{};
    std::uint64_t unreadConfigurations{};
    std::uint64_t malformedDescriptors{};
    std::uint64_t malformedPlacements{};
    std::uint64_t unresolvedClassDefinitions{};
    std::uint64_t droppedLinks{};
    std::uint64_t droppedPlacements{};
    bool complete{};
};

/** One typed ClientRef found directly or at a verified descriptor field. */
struct TypedReference final {
    std::uint32_t sourceObjectRow{};
    std::uint32_t sourceSlotRow{kNoRow};
    std::uint32_t sourceConfigTag{};
    std::uint32_t sourceOffset{};
    std::uint32_t targetObjectRow{kNoRow};
    std::uint32_t targetKey{};
    std::uint16_t targetSlotIndex{};
    std::uint16_t targetSlotType{};
    std::uint32_t sourceConfigNameRow{kNoRow};
    ReferenceJoin join{ReferenceJoin::unresolved};
};

/**
 * One explicitly indexed package transform reached under an object definition.
 * `sourceObjectRow` is provenance only; this row is not a ClientRef or live-object identity.
 */
struct AuthoredPlacement final {
    std::uint32_t sourceObjectRow{};
    std::uint32_t bubbleRow{};
    std::uint32_t stateRow{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t objectListTag{};
    std::uint32_t classListTag{};
    std::uint32_t entryIndex{};
    std::uint32_t objectListNameRow{kNoRow};
    std::uint32_t classListNameRow{kNoRow};
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    std::uint64_t sourceOffset{};
    std::uint64_t identifier{};
    std::int64_t auxiliaryRelative{};
    std::array<std::uint32_t, 4> rotationBits{};
    std::array<std::uint32_t, 3> positionBits{};
    float uniformScale{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    SpatialContextJoin context{SpatialContextJoin::unresolved};
};

/** Exact point-to-placement cardinality in one scenario-owned authored-squad context. */
enum class AuthoredSquadPointContextStatus : std::uint8_t {
    unresolved,
    exact,
    ambiguous,
};

/** One scenario-owned authored spawner/rule configuration occurrence. */
struct AuthoredSquadConfigContext final {
    std::uint32_t globalRow{kNoRow};
    std::uint32_t scenarioIndex{kNoRow};
    std::uint32_t configTag{};
    std::uint32_t occurrenceIndex{kNoRow};
    std::uint32_t objectIndex{kNoRow};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t spawnerRow{kNoRow};
    std::uint32_t ruleRow{kNoRow};
    bool complete{};
};

/** One scenario-owned authored placement occurrence with every stable package field. */
struct AuthoredSquadPlacementContext final {
    std::uint32_t globalRow{kNoRow};
    std::uint32_t scenarioIndex{kNoRow};
    std::uint32_t occurrenceIndex{kNoRow};
    std::uint32_t objectListTag{};
    std::uint32_t placementOrdinal{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
    std::uint32_t objectIndex{kNoRow};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t actorDefinitionTag{kNoRow};
    std::uint64_t sourceOffset{};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::int64_t auxiliaryRelative{};
    bool complete{};
};

/** One scenario-owned authored point/config cardinality with global and local child ranges. */
struct AuthoredSquadPointContext final {
    std::uint32_t globalRow{kNoRow};
    std::uint32_t scenarioIndex{kNoRow};
    std::uint32_t pointRow{kNoRow};
    std::uint32_t globalConfigContextRow{kNoRow};
    std::uint32_t configContextRow{kNoRow};
    std::uint32_t globalFirstMatch{kNoRow};
    std::uint32_t firstMatch{};
    std::uint32_t matchCount{};
    AuthoredSquadPointContextStatus status{AuthoredSquadPointContextStatus::unresolved};
};

/** One exact point-to-placement match with both graph-global and shard-local joins. */
struct AuthoredSquadPointPlacementMatch final {
    std::uint32_t globalRow{kNoRow};
    std::uint32_t scenarioIndex{kNoRow};
    std::uint32_t globalPointContextRow{kNoRow};
    std::uint32_t pointContextRow{kNoRow};
    std::uint32_t pointRow{kNoRow};
    std::uint32_t globalConfigContextRow{kNoRow};
    std::uint32_t configContextRow{kNoRow};
    std::uint32_t globalPlacementContextRow{kNoRow};
    std::uint32_t placementContextRow{kNoRow};
    std::uint64_t placedEntryIdentity{};
    bool sameOccurrence{};
};

/** One scenario membership row owned by an estate-global authored spawner-rule edge. */
struct AuthoredSquadEdgeContext final {
    std::uint32_t globalRow{kNoRow};
    std::uint32_t scenarioIndex{kNoRow};
    std::uint32_t edgeRow{kNoRow};
};

/** One destination-agnostic object list reached through a selected-stem container. */
struct ContainerPlacementList final {
    std::uint32_t objectListTag{};
    std::uint32_t resourceTag{};
    std::uint32_t resourceClass{};
    std::uint32_t objectListNameRow{kNoRow};
    std::uint32_t resourceNameRow{kNoRow};
    bool resourceFieldRead{};
    bool resourceResolved{};
    bool complete{};
};

/** One exact container member edge. Each row retains its own authored bubble mask. */
struct ContainerPlacementOwner final {
    std::uint32_t listRow{};
    std::uint32_t containerTag{};
    std::uint32_t memberIndex{};
    std::uint32_t containerNameRow{kNoRow};
    std::uint64_t scenarioBubbleMask{};
    std::array<std::uint8_t, 32> mapBubbleMask{};
    SpatialContextJoin context{SpatialContextJoin::unresolved};
};

/** One exact package transform keyed by object-list tag and entry index. */
struct ContainerPlacement final {
    std::uint32_t listRow{};
    std::uint32_t objectListTag{};
    std::uint32_t entryIndex{};
    std::uint32_t classListTag{};
    std::uint32_t classListNameRow{kNoRow};
    std::uint32_t firstConfig{};
    std::uint32_t configCount{};
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    float uniformScale{};
    std::uint64_t placementIdentifier{};
    std::uint8_t objectType{};
    bool placementIdentifierRead{};
    bool complete{};
};

/** One exact placement-identifier match and its active owner count. */
struct Type23PlacementCandidate final {
    std::uint32_t linkRow{};
    std::uint32_t placementRow{};
    std::uint32_t ownerRow{kNoRow};
    std::uint32_t applicableOwnerCount{};
};

/** One type-23 descriptor joined to every equal authored placement identifier. */
struct Type23PlacementLink final {
    std::uint32_t descriptorRow{};
    std::uint32_t slotRow{};
    std::uint32_t firstCandidate{};
    std::uint32_t candidateCount{};
    std::uint32_t identityMatchCount{};
    std::uint32_t activeCandidateCount{};
    std::uint32_t resolvedCandidate{kNoRow};
    std::uint64_t placementIdentifier{};
    ReferenceJoin join{ReferenceJoin::unresolved};
    bool complete{};
};

/** Completeness and exact capacity losses of the type-23 placement join. */
struct Type23PlacementDiagnostics final {
    std::uint64_t unreadIdentifiers{};
    std::uint64_t droppedLinks{};
    std::uint64_t droppedCandidates{};
    std::uint64_t zeroIdentityMatches{};
    std::uint64_t multipleIdentityMatches{};
    std::uint64_t zeroActiveCandidates{};
    std::uint64_t multipleActiveCandidates{};
    /** Links resolved by the scenario rule after the sensor's own bubble selected nothing. */
    std::uint64_t scenarioResolvedCandidates{};
    bool complete{};
};

/** One ordered build-row config reached from a placed class definition. */
struct ContainerPlacementConfig final {
    std::uint32_t placementRow{};
    std::uint32_t configTag{};
    std::uint32_t configNameRow{kNoRow};
    std::uint32_t firstComponent{};
    std::uint32_t componentCount{};
    std::uint32_t buildOrdinal{};
    std::uint32_t secondWord{};
    std::uint32_t thirdWord{};
    bool complete{};
};

/** One exact four-word row from a config's typed component array. */
struct ContainerPlacementComponent final {
    std::uint32_t configRow{};
    std::uint32_t componentClass{};
    std::uint32_t firstWord{};
    std::uint32_t secondWord{};
    std::uint32_t fourthWord{};
    std::uint32_t ordinal{};
};

/** Completeness and exact capacity losses of the container-placement pass. */
struct ContainerPlacementDiagnostics final {
    std::uint64_t unresolvedReads{};
    std::uint64_t semanticUnresolved{};
    std::uint64_t droppedLists{};
    std::uint64_t droppedOwners{};
    std::uint64_t droppedPlacements{};
    std::uint64_t droppedConfigs{};
    std::uint64_t droppedComponents{};
    bool contextResolved{};
    bool contextNotApplicable{};
    bool identityOwnerInventoryComplete{};
    bool complete{};
};

/** One class-0x8080966D table reached through a container member's typed package chain. */
struct StaticSpatialTable final {
    std::uint32_t tableTag{};
    std::uint32_t boundsTag{};
    std::uint32_t firstInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t tableNameRow{kNoRow};
    std::uint32_t boundsNameRow{kNoRow};
    bool complete{};
};

/** One exact package chain whose container gives a table scenario-bubble context. */
struct StaticSpatialOwner final {
    std::uint32_t tableRow{};
    std::uint32_t placementRow{kNoRow};
    std::uint32_t containerTag{};
    std::uint32_t objectListTag{};
    std::uint32_t parentTag{};
    std::uint32_t objectListEntry{};
    std::uint32_t containerNameRow{kNoRow};
    std::uint32_t objectListNameRow{kNoRow};
    std::uint32_t parentNameRow{kNoRow};
    std::uint64_t scenarioBubbleMask{};
    std::array<std::uint8_t, 32> mapBubbleMask{};
    SpatialContextJoin context{SpatialContextJoin::unresolved};
};

/** One indexed package spatial row with proved local AABB lanes and opaque transform metadata. */
struct StaticSpatialInstance final {
    std::uint32_t tableRow{};
    std::uint32_t instanceIndex{};
    std::uint32_t resourceTag{};
    std::uint32_t resourceNameRow{kNoRow};
    std::array<float, 4> rotationCandidate{};
    std::array<float, 4> positionCandidate{};
    std::array<float, 4> scaleCandidate{};
    std::array<float, 4> localMinimum{};
    std::array<float, 4> localMaximum{};
    std::array<std::byte, 16> boundsOpaque{};
};

/** One unique class-0x808099C8 root discovered inside a reached placed-object config. */
struct TriggerVolumeTable final {
    std::uint32_t configTag{};
    std::uint32_t configNameRow{kNoRow};
    std::uint32_t firstInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t identityMatchCount{};
    std::uint32_t registryKey{};
    std::uint32_t componentOrdinal{};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
    bool complete{};
};

/** One scenario object occurrence whose exact slot owns a trigger-volume table. */
struct TriggerVolumeOwner final {
    std::uint32_t tableRow{};
    std::uint32_t objectRow{};
    std::uint32_t slotRow{kNoRow};
    std::uint32_t slotMatchCount{};
    std::uint32_t firstIncomingReference{};
    std::uint32_t incomingReferenceCount{};
    std::uint32_t incomingReferenceMatchCount{};
    ReferenceJoin slotJoin{ReferenceJoin::unresolved};
};

/** One state-local incoming type-31 reference attached to a compatible type-60 trigger owner. */
struct TriggerVolumeIncomingReference final {
    std::uint32_t ownerRow{};
    std::uint32_t referenceRow{};
    std::uint32_t sourceObjectRow{};
    std::uint32_t sourceSlotRow{};
};

/** One world-coordinate vertex retained from a proved native physics-shape input. */
struct TriggerVolumeVertex final {
    std::array<float, 4> value{};
};

/** One tightly packed triangle from a proved native physics-shape input. */
struct TriggerVolumeTriangle final {
    std::array<std::uint8_t, 3> indices{};
};

/** One exact key/type/index-matched authored trigger-volume row. */
struct TriggerVolumeInstance final {
    std::uint32_t tableRow{};
    std::uint32_t authoredRowIndex{};
    std::uint32_t classDefinitionTag{};
    std::uint32_t classDefinitionNameRow{kNoRow};
    std::uint32_t shapeResourceTag{};
    std::uint32_t shapeResourceNameRow{kNoRow};
    std::uint32_t shapeReferenceWord{};
    std::uint32_t shapeIndex{};
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::uint32_t firstTriangle{};
    std::uint32_t triangleCount{};
    std::uint32_t flags{};
    std::array<float, 4> rotation{};
    std::array<float, 4> position{};
    std::array<float, 4> minimum{};
    std::array<float, 4> maximum{};
    float extrusion{};
    std::uint8_t active{};
    bool complete{};
};

/** Completeness and exact capacity losses of the trigger-volume extraction pass. */
struct TriggerVolumeDiagnostics final {
    std::uint64_t unresolvedReads{};
    std::uint64_t droppedTables{};
    std::uint64_t droppedOwners{};
    std::uint64_t droppedInstances{};
    std::uint64_t droppedVertices{};
    std::uint64_t droppedTriangles{};
    std::uint64_t droppedIncomingReferences{};
    std::uint64_t zeroMatches{};
    std::uint64_t multipleMatches{};
    bool complete{};
};

/** One package-derived name candidate with its exact source tuple and evidence tier. */
struct NameCandidate final {
    std::array<char, kNameCapacity> value{};
    std::uint32_t sourceTag{};
    std::uint32_t sourceClassId{};
    std::uint16_t length{};
    NameProvenance provenance{NameProvenance::unresolved};
};

/** One hash and its candidate range. Selection requires one distinct value at the best tier. */
struct Name final {
    std::uint32_t hash{};
    std::uint32_t firstCandidate{};
    std::uint32_t candidateCount{};
    std::uint32_t selectedCandidate{kNoRow};
    NameProvenance provenance{NameProvenance::unresolved};
    bool strongestTierOverflow{};
};

/** One FNV-1 checked UTF-8 value inside the snapshot's raw inline-name byte bank. */
struct InlineNameCandidate final {
    std::uint32_t hash{};
    std::uint32_t firstByte{};
    std::uint32_t byteCount{};
};

/**
 * One exact package definition tag and its named-definition candidates.
 * `classId == 0` means the caller had no proved class constraint.
 */
struct TagName final {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint32_t firstCandidate{};
    std::uint32_t candidateCount{};
    std::uint32_t selectedCandidate{kNoRow};
    NameProvenance provenance{NameProvenance::unresolved};
};

/** Immutable, process-only result for one selected installed scenario. */
struct Snapshot final {
    std::uint64_t revision{};
    std::uint64_t request{};
    std::uint32_t scenarioTag{};
    std::array<char, kScenarioNameCapacity> scenarioName{};
    std::uint8_t scenarioNameLength{};
    BuildStatus status{BuildStatus::idle};
    BuildCoverage coverage{BuildCoverage::none};
    std::array<char, 96> detail{};
    CoverageDiagnostics coverageDiagnostics{};
    std::vector<Bubble> bubbles{};
    std::vector<State> states{};
    std::vector<Object> objects{};
    std::vector<Slot> slots{};
    std::vector<Descriptor> descriptors{};
    std::vector<EmbeddedPlacementLink> embeddedPlacementLinks{};
    std::vector<EmbeddedPlacement> embeddedPlacements{};
    std::vector<TypedReference> references{};
    std::vector<AuthoredPlacement> authoredPlacements{};
    std::vector<ContainerPlacementList> containerPlacementLists{};
    std::vector<ContainerPlacementOwner> containerPlacementOwners{};
    std::vector<ContainerPlacement> containerPlacements{};
    std::vector<ContainerPlacementConfig> containerPlacementConfigs{};
    std::vector<ContainerPlacementComponent> containerPlacementComponents{};
    std::vector<Type23PlacementLink> type23PlacementLinks{};
    std::vector<Type23PlacementCandidate> type23PlacementCandidates{};
    std::vector<StaticSpatialTable> staticSpatialTables{};
    std::vector<StaticSpatialOwner> staticSpatialOwners{};
    std::vector<StaticSpatialInstance> staticSpatialInstances{};
    std::vector<TriggerVolumeTable> triggerVolumeTables{};
    std::vector<TriggerVolumeOwner> triggerVolumeOwners{};
    std::vector<TriggerVolumeIncomingReference> triggerVolumeIncomingReferences{};
    std::vector<TriggerVolumeInstance> triggerVolumeInstances{};
    std::vector<TriggerVolumeVertex> triggerVolumeVertices{};
    std::vector<TriggerVolumeTriangle> triggerVolumeTriangles{};
    std::vector<Name> names{};
    std::vector<TagName> tagNames{};
    std::vector<NameCandidate> nameCandidates{};
    std::vector<InlineNameCandidate> inlineNameCandidates{};
    std::vector<std::byte> inlineNameBytes{};
    std::vector<AuthoredSquadConfigContext> authoredSquadConfigContexts{};
    std::vector<AuthoredSquadPlacementContext> authoredSquadPlacementContexts{};
    std::vector<AuthoredSquadPointContext> authoredSquadPointContexts{};
    std::vector<AuthoredSquadPointPlacementMatch> authoredSquadPointPlacementMatches{};
    std::vector<AuthoredSquadEdgeContext> authoredSquadEdgeContexts{};
    std::uint64_t unresolvedReads{};
    ContainerPlacementDiagnostics containerPlacementDiagnostics{};
    Type23PlacementDiagnostics type23PlacementDiagnostics{};
    EmbeddedPlacementDiagnostics embeddedPlacementDiagnostics{};
    std::uint64_t staticSpatialUnresolvedReads{};
    std::uint64_t staticSpatialSemanticUnresolved{};
    std::uint64_t staticSpatialDropped{};
    bool staticSpatialContextResolved{};
    bool staticSpatialNotApplicable{};
    bool staticSpatialComplete{};
    TriggerVolumeDiagnostics triggerVolumeDiagnostics{};
    bool authoredSquadGraphContextsComplete{};
};

} // namespace sunrise::state::build_data::scriptables
