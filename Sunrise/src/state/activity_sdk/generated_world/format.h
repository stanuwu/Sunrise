#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "codec.h"

namespace sunrise::state::activity_sdk::generated_world::format {

namespace catalog = build_data::scriptables;

/** Generated-world scenario-shard identity. */
inline constexpr std::array<char, 8> kMagic{'S', 'R', 'G', 'W', 'S', 'H', 'R', 'D'};
/** Shard schema. Increment for row-layout or extraction-semantic changes. */
// Invalidate cached graph edges carrying the former single-rule association result.
inline constexpr std::uint32_t kVersion = 14;
/** No extracted section may exceed the largest bounded package-derived catalog bank. */
inline constexpr std::uint32_t kMaximumRowsPerSection = 1'048'576;
/** A corrupt local shard cannot ask the loader for an unbounded allocation. */
inline constexpr std::uint64_t kMaximumFileSize = 128ULL * 1024ULL * 1024ULL;
/** Raw UTF-8 bytes may use the authenticated file cap instead of the typed-row cap. */
inline constexpr std::uint32_t kMaximumInlineNameBankBytes =
    static_cast<std::uint32_t>(kMaximumFileSize);
/** Version-seven and later authored placements retain every stable package-row field. */
inline constexpr std::size_t kAuthoredPlacementStride = 136;

/** Fixed order of every vector owned by `scriptables::Snapshot`. */
enum class SectionIndex : std::size_t {
    bubbles,
    states,
    objects,
    slots,
    descriptors,
    embeddedPlacementLinks,
    embeddedPlacements,
    references,
    authoredPlacements,
    containerPlacementLists,
    containerPlacementOwners,
    containerPlacements,
    containerPlacementConfigs,
    containerPlacementComponents,
    type23PlacementLinks,
    type23PlacementCandidates,
    staticSpatialTables,
    staticSpatialOwners,
    staticSpatialInstances,
    triggerVolumeTables,
    triggerVolumeOwners,
    triggerVolumeIncomingReferences,
    triggerVolumeInstances,
    triggerVolumeVertices,
    triggerVolumeTriangles,
    names,
    tagNames,
    nameCandidates,
    inlineNameCandidates,
    inlineNameBytes,
    authoredSquadConfigContexts,
    authoredSquadPlacementContexts,
    authoredSquadPointContexts,
    authoredSquadPointPlacementMatches,
    authoredSquadEdgeContexts,
    count,
};

inline constexpr std::size_t kSectionCount = static_cast<std::size_t>(SectionIndex::count);
static_assert(kSectionCount == 35);

#pragma pack(push, 1)

/** One fixed-stride vector section inside the payload. */
struct Section final {
    std::uint64_t offset{};
    std::uint32_t count{};
    std::uint32_t stride{};
};

/** Packed structural-family result without native enum representation. */
struct FamilyCoverageDiagnostic final {
    std::uint8_t status{};
    std::uint8_t lossMask{};
};

/** Every non-vector scalar from the container-placement diagnostic group. */
struct ContainerPlacementDiagnostics final {
    std::uint64_t unresolvedReads{};
    std::uint64_t semanticUnresolved{};
    std::uint64_t droppedLists{};
    std::uint64_t droppedOwners{};
    std::uint64_t droppedPlacements{};
    std::uint64_t droppedConfigs{};
    std::uint64_t droppedComponents{};
    std::uint8_t contextResolved{};
    std::uint8_t contextNotApplicable{};
    std::uint8_t identityOwnerInventoryComplete{};
    std::uint8_t complete{};
};

/** Every non-vector scalar from the type-23 diagnostic group. */
struct Type23PlacementDiagnostics final {
    std::uint64_t unreadIdentifiers{};
    std::uint64_t droppedLinks{};
    std::uint64_t droppedCandidates{};
    std::uint64_t zeroIdentityMatches{};
    std::uint64_t multipleIdentityMatches{};
    std::uint64_t zeroActiveCandidates{};
    std::uint64_t multipleActiveCandidates{};
    std::uint8_t complete{};
};

/** Every non-vector scalar from the embedded-placement diagnostic group. */
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
    std::uint8_t complete{};
};

/** Every non-vector scalar from the trigger-volume diagnostic group. */
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
    std::uint8_t complete{};
};

/** Every non-vector field of one process snapshot, encoded without native bool padding. */
struct Scalars final {
    std::uint64_t revision{};
    std::uint64_t request{};
    std::uint32_t scenarioTag{};
    std::array<char, catalog::kScenarioNameCapacity> scenarioName{};
    std::uint8_t scenarioNameLength{};
    std::uint8_t status{};
    std::uint8_t coverage{};
    std::array<char, 96> detail{};
    std::array<FamilyCoverageDiagnostic, catalog::kStructuralFamilyCount> coverageDiagnostics{};
    std::uint64_t unresolvedReads{};
    ContainerPlacementDiagnostics containerPlacementDiagnostics{};
    Type23PlacementDiagnostics type23PlacementDiagnostics{};
    EmbeddedPlacementDiagnostics embeddedPlacementDiagnostics{};
    std::uint64_t staticSpatialUnresolvedReads{};
    std::uint64_t staticSpatialSemanticUnresolved{};
    std::uint64_t staticSpatialDropped{};
    std::uint8_t staticSpatialContextResolved{};
    std::uint8_t staticSpatialNotApplicable{};
    std::uint8_t staticSpatialComplete{};
    TriggerVolumeDiagnostics triggerVolumeDiagnostics{};
    std::uint8_t authoredSquadGraphContextsComplete{};
    std::array<std::uint8_t, 5> reserved{};
};

/** Header binding one complete payload to a scenario and installed-content fingerprint. */
struct Header final {
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint64_t fileSize{};
    std::uint32_t scenarioTag{};
    std::uint32_t sectionCount{};
    std::uint32_t scalarSize{};
    std::uint32_t reserved{};
    Digest sourceFingerprint{};
    Digest payloadSha256{};
    std::array<Section, kSectionCount> sections{};
};

#pragma pack(pop)

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(std::size_t) == sizeof(std::uint64_t));
static_assert(sizeof(bool) == sizeof(std::uint8_t));
static_assert(sizeof(Section) == 16);
static_assert(sizeof(FamilyCoverageDiagnostic) == 2);
static_assert(sizeof(catalog::InlineNameCandidate) == 12);
static_assert(sizeof(Header) == 664);
static_assert(sizeof(Scalars) == 507);
static_assert(sizeof(catalog::AuthoredPlacement) == kAuthoredPlacementStride);
static_assert(sizeof(catalog::AuthoredSquadConfigContext) == 40);
static_assert(sizeof(catalog::AuthoredSquadPlacementContext) == 120);
static_assert(sizeof(catalog::AuthoredSquadPointContext) == 36);
static_assert(sizeof(catalog::AuthoredSquadPointPlacementMatch) == 56);
static_assert(sizeof(catalog::AuthoredSquadEdgeContext) == 12);
static_assert(std::is_trivially_copyable_v<Header> && std::is_standard_layout_v<Header>);
static_assert(std::is_trivially_copyable_v<Scalars> && std::is_standard_layout_v<Scalars>);

static_assert(std::is_trivially_copyable_v<catalog::Bubble>
              && std::is_standard_layout_v<catalog::Bubble>);
static_assert(std::is_trivially_copyable_v<catalog::State>
              && std::is_standard_layout_v<catalog::State>);
static_assert(std::is_trivially_copyable_v<catalog::Object>
              && std::is_standard_layout_v<catalog::Object>);
static_assert(std::is_trivially_copyable_v<catalog::Slot>
              && std::is_standard_layout_v<catalog::Slot>);
static_assert(std::is_trivially_copyable_v<catalog::Descriptor>
              && std::is_standard_layout_v<catalog::Descriptor>);
static_assert(std::is_trivially_copyable_v<catalog::EmbeddedPlacementLink>
              && std::is_standard_layout_v<catalog::EmbeddedPlacementLink>);
static_assert(std::is_trivially_copyable_v<catalog::EmbeddedPlacement>
              && std::is_standard_layout_v<catalog::EmbeddedPlacement>);
static_assert(std::is_trivially_copyable_v<catalog::TypedReference>
              && std::is_standard_layout_v<catalog::TypedReference>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredPlacement>
              && std::is_standard_layout_v<catalog::AuthoredPlacement>);
static_assert(std::is_trivially_copyable_v<catalog::ContainerPlacementList>
              && std::is_standard_layout_v<catalog::ContainerPlacementList>);
static_assert(std::is_trivially_copyable_v<catalog::ContainerPlacementOwner>
              && std::is_standard_layout_v<catalog::ContainerPlacementOwner>);
static_assert(std::is_trivially_copyable_v<catalog::ContainerPlacement>
              && std::is_standard_layout_v<catalog::ContainerPlacement>);
static_assert(std::is_trivially_copyable_v<catalog::ContainerPlacementConfig>
              && std::is_standard_layout_v<catalog::ContainerPlacementConfig>);
static_assert(std::is_trivially_copyable_v<catalog::ContainerPlacementComponent>
              && std::is_standard_layout_v<catalog::ContainerPlacementComponent>);
static_assert(std::is_trivially_copyable_v<catalog::Type23PlacementLink>
              && std::is_standard_layout_v<catalog::Type23PlacementLink>);
static_assert(std::is_trivially_copyable_v<catalog::Type23PlacementCandidate>
              && std::is_standard_layout_v<catalog::Type23PlacementCandidate>);
static_assert(std::is_trivially_copyable_v<catalog::StaticSpatialTable>
              && std::is_standard_layout_v<catalog::StaticSpatialTable>);
static_assert(std::is_trivially_copyable_v<catalog::StaticSpatialOwner>
              && std::is_standard_layout_v<catalog::StaticSpatialOwner>);
static_assert(std::is_trivially_copyable_v<catalog::StaticSpatialInstance>
              && std::is_standard_layout_v<catalog::StaticSpatialInstance>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeTable>
              && std::is_standard_layout_v<catalog::TriggerVolumeTable>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeOwner>
              && std::is_standard_layout_v<catalog::TriggerVolumeOwner>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeIncomingReference>
              && std::is_standard_layout_v<catalog::TriggerVolumeIncomingReference>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeInstance>
              && std::is_standard_layout_v<catalog::TriggerVolumeInstance>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeVertex>
              && std::is_standard_layout_v<catalog::TriggerVolumeVertex>);
static_assert(std::is_trivially_copyable_v<catalog::TriggerVolumeTriangle>
              && std::is_standard_layout_v<catalog::TriggerVolumeTriangle>);
static_assert(std::is_trivially_copyable_v<catalog::Name>
              && std::is_standard_layout_v<catalog::Name>);
static_assert(std::is_trivially_copyable_v<catalog::TagName>
              && std::is_standard_layout_v<catalog::TagName>);
static_assert(std::is_trivially_copyable_v<catalog::NameCandidate>
              && std::is_standard_layout_v<catalog::NameCandidate>);
static_assert(std::is_trivially_copyable_v<catalog::InlineNameCandidate>
              && std::is_standard_layout_v<catalog::InlineNameCandidate>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredSquadConfigContext>
              && std::is_standard_layout_v<catalog::AuthoredSquadConfigContext>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredSquadPlacementContext>
              && std::is_standard_layout_v<catalog::AuthoredSquadPlacementContext>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredSquadPointContext>
              && std::is_standard_layout_v<catalog::AuthoredSquadPointContext>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredSquadPointPlacementMatch>
              && std::is_standard_layout_v<catalog::AuthoredSquadPointPlacementMatch>);
static_assert(std::is_trivially_copyable_v<catalog::AuthoredSquadEdgeContext>
              && std::is_standard_layout_v<catalog::AuthoredSquadEdgeContext>);

} // namespace sunrise::state::activity_sdk::generated_world::format
