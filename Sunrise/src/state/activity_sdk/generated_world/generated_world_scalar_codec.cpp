#include "generated_world_scalar_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity_sdk::generated_world::internal {

namespace catalog = build_data::scriptables;

namespace {

/** Encodes one native bool without exposing an implementation-specific representation. */
[[nodiscard]] constexpr std::uint8_t flag(bool value) noexcept {
    return value ? 1U : 0U;
}

} // namespace

/** Copies every non-vector snapshot field into its packed disk representation. */
[[nodiscard]] format::Scalars encode_scalars(const catalog::Snapshot& snapshot) noexcept {
    format::Scalars output{};
    output.revision = snapshot.revision;
    output.request = snapshot.request;
    output.scenarioTag = snapshot.scenarioTag;
    output.scenarioName = snapshot.scenarioName;
    output.scenarioNameLength = snapshot.scenarioNameLength;
    output.status = static_cast<std::uint8_t>(snapshot.status);
    output.coverage = static_cast<std::uint8_t>(snapshot.coverage);
    output.detail = snapshot.detail;
    for (std::size_t index = 0; index < output.coverageDiagnostics.size(); ++index) {
        const catalog::FamilyCoverageDiagnostic& source =
            snapshot.coverageDiagnostics.families[index];
        output.coverageDiagnostics[index] = {static_cast<std::uint8_t>(source.status),
                                             source.lossMask};
    }
    output.unresolvedReads = snapshot.unresolvedReads;

    const auto& container = snapshot.containerPlacementDiagnostics;
    output.containerPlacementDiagnostics = {
        container.unresolvedReads,
        container.semanticUnresolved,
        container.droppedLists,
        container.droppedOwners,
        container.droppedPlacements,
        container.droppedConfigs,
        container.droppedComponents,
        flag(container.contextResolved),
        flag(container.contextNotApplicable),
        flag(container.identityOwnerInventoryComplete),
        flag(container.complete),
    };
    const auto& type23 = snapshot.type23PlacementDiagnostics;
    output.type23PlacementDiagnostics = {
        type23.unreadIdentifiers,
        type23.droppedLinks,
        type23.droppedCandidates,
        type23.zeroIdentityMatches,
        type23.multipleIdentityMatches,
        type23.zeroActiveCandidates,
        type23.multipleActiveCandidates,
        flag(type23.complete),
    };
    const auto& embedded = snapshot.embeddedPlacementDiagnostics;
    output.embeddedPlacementDiagnostics = {
        embedded.applicableDescriptors,
        embedded.emptyDescriptors,
        embedded.readPlacements,
        embedded.unreadConfigurations,
        embedded.malformedDescriptors,
        embedded.malformedPlacements,
        embedded.unresolvedClassDefinitions,
        embedded.droppedLinks,
        embedded.droppedPlacements,
        flag(embedded.complete),
    };
    output.staticSpatialUnresolvedReads = snapshot.staticSpatialUnresolvedReads;
    output.staticSpatialSemanticUnresolved = snapshot.staticSpatialSemanticUnresolved;
    output.staticSpatialDropped = snapshot.staticSpatialDropped;
    output.staticSpatialContextResolved = flag(snapshot.staticSpatialContextResolved);
    output.staticSpatialNotApplicable = flag(snapshot.staticSpatialNotApplicable);
    output.staticSpatialComplete = flag(snapshot.staticSpatialComplete);
    const auto& trigger = snapshot.triggerVolumeDiagnostics;
    output.triggerVolumeDiagnostics = {
        trigger.unresolvedReads,
        trigger.droppedTables,
        trigger.droppedOwners,
        trigger.droppedInstances,
        trigger.droppedVertices,
        trigger.droppedTriangles,
        trigger.droppedIncomingReferences,
        trigger.zeroMatches,
        trigger.multipleMatches,
        flag(trigger.complete),
    };
    output.authoredSquadGraphContextsComplete = flag(snapshot.authoredSquadGraphContextsComplete);
    return output;
}

/** @return True when packed scalar enums, flags, strings, and reserved bytes are canonical. */
[[nodiscard]] bool valid_scalars(const format::Scalars& value) noexcept {
    const auto validFlag = [](std::uint8_t candidate) noexcept { return candidate <= 1U; };
    const auto& container = value.containerPlacementDiagnostics;
    const auto& type23 = value.type23PlacementDiagnostics;
    const auto& embedded = value.embeddedPlacementDiagnostics;
    const auto& trigger = value.triggerVolumeDiagnostics;
    for (const format::FamilyCoverageDiagnostic& family : value.coverageDiagnostics) {
        const bool incomplete =
            family.status == static_cast<std::uint8_t>(catalog::FamilyCoverageStatus::incomplete);
        if (family.status > static_cast<std::uint8_t>(catalog::FamilyCoverageStatus::incomplete)
            || (family.lossMask
                & static_cast<std::uint8_t>(~(catalog::kFamilyCoverageLossUnread
                                              | catalog::kFamilyCoverageLossDropped
                                              | catalog::kFamilyCoverageLossPartial)))
                   != 0
            || incomplete != (family.lossMask != catalog::kFamilyCoverageLossNone)) {
            return false;
        }
    }
    if (value.scenarioTag == 0 || value.scenarioNameLength == 0
        || value.scenarioNameLength >= value.scenarioName.size()
        || value.status > static_cast<std::uint8_t>(catalog::BuildStatus::failed)
        || value.coverage > static_cast<std::uint8_t>(catalog::BuildCoverage::full)
        || (value.status == static_cast<std::uint8_t>(catalog::BuildStatus::ready)
            && value.coverage == static_cast<std::uint8_t>(catalog::BuildCoverage::none))
        || value.scenarioName[value.scenarioNameLength] != '\0'
        || std::find(value.scenarioName.begin(),
                     value.scenarioName.begin() + value.scenarioNameLength,
                     '\0')
               != value.scenarioName.begin() + value.scenarioNameLength
        || std::find(value.detail.begin(), value.detail.end(), '\0') == value.detail.end()
        || !validFlag(container.contextResolved) || !validFlag(container.contextNotApplicable)
        || !validFlag(container.identityOwnerInventoryComplete) || !validFlag(container.complete)
        || !validFlag(type23.complete) || !validFlag(embedded.complete)
        || !validFlag(value.staticSpatialContextResolved)
        || !validFlag(value.staticSpatialNotApplicable) || !validFlag(value.staticSpatialComplete)
        || !validFlag(trigger.complete) || !validFlag(value.authoredSquadGraphContextsComplete)) {
        return false;
    }
    return std::all_of(
        value.reserved.begin(), value.reserved.end(), [](std::uint8_t byte) { return byte == 0; });
}

/** Decodes every checked packed scalar into one otherwise-empty native snapshot. */
void decode_scalars(const format::Scalars& value, catalog::Snapshot& snapshot) noexcept {
    snapshot.revision = value.revision;
    snapshot.request = value.request;
    snapshot.scenarioTag = value.scenarioTag;
    snapshot.scenarioName = value.scenarioName;
    snapshot.scenarioNameLength = value.scenarioNameLength;
    snapshot.status = static_cast<catalog::BuildStatus>(value.status);
    snapshot.coverage = static_cast<catalog::BuildCoverage>(value.coverage);
    snapshot.detail = value.detail;
    for (std::size_t index = 0; index < value.coverageDiagnostics.size(); ++index) {
        const format::FamilyCoverageDiagnostic& source = value.coverageDiagnostics[index];
        snapshot.coverageDiagnostics.families[index] = {
            static_cast<catalog::FamilyCoverageStatus>(source.status), source.lossMask};
    }
    snapshot.unresolvedReads = value.unresolvedReads;

    const auto& container = value.containerPlacementDiagnostics;
    snapshot.containerPlacementDiagnostics = {
        container.unresolvedReads,
        container.semanticUnresolved,
        container.droppedLists,
        container.droppedOwners,
        container.droppedPlacements,
        container.droppedConfigs,
        container.droppedComponents,
        container.contextResolved != 0,
        container.contextNotApplicable != 0,
        container.identityOwnerInventoryComplete != 0,
        container.complete != 0,
    };
    const auto& type23 = value.type23PlacementDiagnostics;
    // This disk format does not store the scenario fallback count.
    snapshot.type23PlacementDiagnostics = {
        .unreadIdentifiers = type23.unreadIdentifiers,
        .droppedLinks = type23.droppedLinks,
        .droppedCandidates = type23.droppedCandidates,
        .zeroIdentityMatches = type23.zeroIdentityMatches,
        .multipleIdentityMatches = type23.multipleIdentityMatches,
        .zeroActiveCandidates = type23.zeroActiveCandidates,
        .multipleActiveCandidates = type23.multipleActiveCandidates,
        .complete = type23.complete != 0,
    };
    const auto& embedded = value.embeddedPlacementDiagnostics;
    snapshot.embeddedPlacementDiagnostics = {
        embedded.applicableDescriptors,
        embedded.emptyDescriptors,
        embedded.readPlacements,
        embedded.unreadConfigurations,
        embedded.malformedDescriptors,
        embedded.malformedPlacements,
        embedded.unresolvedClassDefinitions,
        embedded.droppedLinks,
        embedded.droppedPlacements,
        embedded.complete != 0,
    };
    snapshot.staticSpatialUnresolvedReads = value.staticSpatialUnresolvedReads;
    snapshot.staticSpatialSemanticUnresolved = value.staticSpatialSemanticUnresolved;
    snapshot.staticSpatialDropped = value.staticSpatialDropped;
    snapshot.staticSpatialContextResolved = value.staticSpatialContextResolved != 0;
    snapshot.staticSpatialNotApplicable = value.staticSpatialNotApplicable != 0;
    snapshot.staticSpatialComplete = value.staticSpatialComplete != 0;
    const auto& trigger = value.triggerVolumeDiagnostics;
    snapshot.triggerVolumeDiagnostics = {
        trigger.unresolvedReads,
        trigger.droppedTables,
        trigger.droppedOwners,
        trigger.droppedInstances,
        trigger.droppedVertices,
        trigger.droppedTriangles,
        trigger.droppedIncomingReferences,
        trigger.zeroMatches,
        trigger.multipleMatches,
        trigger.complete != 0,
    };
    snapshot.authoredSquadGraphContextsComplete = value.authoredSquadGraphContextsComplete != 0;
}

} // namespace sunrise::state::activity_sdk::generated_world::internal
