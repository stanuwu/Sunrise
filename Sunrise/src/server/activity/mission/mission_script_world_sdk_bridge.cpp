#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

#include "../../../middleware/crypto/sha256.h"
#include "../../../state/build_data/scriptables/coverage.h"
#include "mission_script_actor_sequence_bridge.h"
#include "mission_script_manifest_sdk_bridge.h"
#include "mission_script_sdk_bridge.h"
#include "mission_script_world_sdk_internal.h"
#include "mission_script_world_sdk_values.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {

namespace sdk = state::activity_sdk;
namespace generated = state::activity_sdk::generated_world;
namespace catalog = state::build_data::scriptables;
namespace crypto = middleware::crypto::sha256;

// Program generation is hashed over this domain string, a separator byte, five 32-byte
// digests, and the 4-byte scenario tag.
constexpr std::string_view kProgramGenerationDomain = "sunrise.mission.world-generation.v1";
constexpr std::size_t kGenerationDigestCount = 5;
constexpr std::size_t kScenarioTagBytes = 4;
constexpr std::size_t kProgramGenerationBytes =
    kProgramGenerationDomain.size() + 1 + kGenerationDigestCount * 32 + kScenarioTagBytes;

[[nodiscard]] std::string_view bounded_text(const char* value, std::size_t capacity) noexcept {
    if (value == nullptr) {
        return {};
    }
    const void* const end = std::memchr(value, '\0', capacity);
    return {value, end == nullptr ? capacity : static_cast<const char*>(end) - value};
}

[[nodiscard]] bool validate_world(const void* context,
                                  const lua_vm::WorldGenerationIdentity& generation) noexcept {
    return checked_world(context, generation) != nullptr;
}

/** @return Total squad anchors across the bound scenario, or 0 on overflow or no binding. */
[[nodiscard]] std::size_t scenario_anchor_count(const sdk::BoundView& view) noexcept {
    if (view.catalog == nullptr || sdk::bound_scenario(view) == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const sdk::format::Squad& squad :
         sdk::scenario_squads(*view.catalog, *sdk::bound_scenario(view))) {
        const std::size_t anchors = sdk::squad_anchors(*view.catalog, squad).size();
        if (anchors > (std::numeric_limits<std::size_t>::max)() - count) {
            return 0;
        }
        count += anchors;
    }
    return count;
}

/** Copies one anchor and converts its raw IEEE-754 lanes without changing bits. */
[[nodiscard]] bool copy_anchor(const sdk::BoundView& view,
                               const sdk::format::Squad& squad,
                               const sdk::format::SquadAnchor& anchor,
                               std::uint32_t squadRow,
                               std::uint32_t collectionRow,
                               lua_vm::SquadAnchorDefinition& output) noexcept {
    output = {};
    const auto allSquads = view.catalog->squads();
    const auto allAnchors = view.catalog->squad_anchors();
    if (&squad < allSquads.data() || &squad >= allSquads.data() + allSquads.size()
        || &anchor < allAnchors.data() || &anchor >= allAnchors.data() + allAnchors.size()) {
        return false;
    }
    const std::uint32_t squadIndex = static_cast<std::uint32_t>(&squad - allSquads.data());
    if (anchor.squadIndex != squadIndex) {
        return false;
    }
    output.id = view.catalog->string(anchor.id);
    for (std::size_t index = 0; index < output.position.size(); ++index) {
        output.position[index] = std::bit_cast<float>(anchor.positionBits[index]);
    }
    output.placedEntryIdentity = anchor.placedEntryIdentity;
    output.row = static_cast<std::uint32_t>(&anchor - allAnchors.data()) + 1U;
    output.collectionRow = collectionRow;
    output.squadRow = squadRow;
    output.pointOrdinal = anchor.pointOrdinal;
    output.objectListTag = anchor.objectListTag;
    output.placementOrdinal = anchor.placementOrdinal;
    output.flags = anchor.flags;
    return !output.id.empty();
}

/** Resolves one 1-based anchor of the bound scenario, counted across all its squads. */
[[nodiscard]] bool resolve_scenario_anchor(const sdk::BoundView& view,
                                           std::uint32_t localRow,
                                           lua_vm::SquadAnchorDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || view.catalog == nullptr || sdk::bound_scenario(view) == nullptr) {
        return false;
    }
    std::uint32_t cursor = 0;
    const auto squads = sdk::scenario_squads(*view.catalog, *sdk::bound_scenario(view));
    for (std::size_t squadIndex = 0; squadIndex < squads.size(); ++squadIndex) {
        for (const sdk::format::SquadAnchor& anchor :
             sdk::squad_anchors(*view.catalog, squads[squadIndex])) {
            if (cursor == (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            ++cursor;
            if (cursor == localRow) {
                return copy_anchor(view,
                                   squads[squadIndex],
                                   anchor,
                                   static_cast<std::uint32_t>(squadIndex + 1U),
                                   cursor,
                                   output);
            }
        }
    }
    return false;
}

/** @return Row count of one world collection kind, or 0 when the generation is stale. */
[[nodiscard]] std::size_t world_count_impl(const void* context,
                                           const lua_vm::WorldGenerationIdentity& generation,
                                           lua_vm::WorldCollectionKind kind) noexcept {
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr) {
        return 0;
    }
    const catalog::Snapshot& snapshot = *world->snapshot();
    switch (kind) {
    case lua_vm::WorldCollectionKind::squadAnchors:
        return scenario_anchor_count(world->activity_sdk_view());
    case lua_vm::WorldCollectionKind::bubbles:
        return snapshot.bubbles.size();
    case lua_vm::WorldCollectionKind::states:
        return snapshot.states.size();
    case lua_vm::WorldCollectionKind::objects:
        return snapshot.objects.size();
    case lua_vm::WorldCollectionKind::slots:
        return snapshot.slots.size();
    case lua_vm::WorldCollectionKind::descriptors:
        return snapshot.descriptors.size();
    case lua_vm::WorldCollectionKind::embeddedPlacementLinks:
        return snapshot.embeddedPlacementLinks.size();
    case lua_vm::WorldCollectionKind::authoredPlacements:
        return snapshot.authoredPlacements.size();
    case lua_vm::WorldCollectionKind::embeddedPlacements:
        return snapshot.embeddedPlacements.size();
    case lua_vm::WorldCollectionKind::typedReferences:
        return snapshot.references.size();
    case lua_vm::WorldCollectionKind::containerPlacementLists:
        return snapshot.containerPlacementLists.size();
    case lua_vm::WorldCollectionKind::containerPlacementOwners:
        return snapshot.containerPlacementOwners.size();
    case lua_vm::WorldCollectionKind::containerPlacements:
        return snapshot.containerPlacements.size();
    case lua_vm::WorldCollectionKind::containerPlacementConfigs:
        return snapshot.containerPlacementConfigs.size();
    case lua_vm::WorldCollectionKind::containerPlacementComponents:
        return snapshot.containerPlacementComponents.size();
    case lua_vm::WorldCollectionKind::type23PlacementLinks:
        return snapshot.type23PlacementLinks.size();
    case lua_vm::WorldCollectionKind::type23PlacementCandidates:
        return snapshot.type23PlacementCandidates.size();
    case lua_vm::WorldCollectionKind::staticSpatialTables:
        return snapshot.staticSpatialTables.size();
    case lua_vm::WorldCollectionKind::staticSpatialOwners:
        return snapshot.staticSpatialOwners.size();
    case lua_vm::WorldCollectionKind::staticSpatialInstances:
        return snapshot.staticSpatialInstances.size();
    case lua_vm::WorldCollectionKind::triggerVolumeTables:
        return snapshot.triggerVolumeTables.size();
    case lua_vm::WorldCollectionKind::triggerVolumeOwners:
        return snapshot.triggerVolumeOwners.size();
    case lua_vm::WorldCollectionKind::triggerVolumeIncomingReferences:
        return snapshot.triggerVolumeIncomingReferences.size();
    case lua_vm::WorldCollectionKind::triggerVolumes:
        return snapshot.triggerVolumeInstances.size();
    case lua_vm::WorldCollectionKind::triggerVolumeVertices:
        return snapshot.triggerVolumeVertices.size();
    case lua_vm::WorldCollectionKind::triggerVolumeTriangles:
        return snapshot.triggerVolumeTriangles.size();
    case lua_vm::WorldCollectionKind::names:
        return snapshot.names.size();
    case lua_vm::WorldCollectionKind::tagNames:
        return snapshot.tagNames.size();
    case lua_vm::WorldCollectionKind::nameCandidates:
        return snapshot.nameCandidates.size();
    case lua_vm::WorldCollectionKind::inlineNameCandidates:
        return snapshot.inlineNameCandidates.size();
    case lua_vm::WorldCollectionKind::authoredSquadConfigContexts:
        return snapshot.authoredSquadConfigContexts.size();
    case lua_vm::WorldCollectionKind::authoredSquadPlacementContexts:
        return snapshot.authoredSquadPlacementContexts.size();
    case lua_vm::WorldCollectionKind::authoredSquadPointContexts:
        return snapshot.authoredSquadPointContexts.size();
    case lua_vm::WorldCollectionKind::authoredSquadPointPlacementMatches:
        return snapshot.authoredSquadPointPlacementMatches.size();
    case lua_vm::WorldCollectionKind::authoredSquadEdgeContexts:
        return snapshot.authoredSquadEdgeContexts.size();
    }
    return 0;
}

/** Copies one 1-based authored placement row into its Lua-facing definition. */
[[nodiscard]] bool copy_authored(const catalog::Snapshot& snapshot,
                                 std::uint32_t localRow,
                                 lua_vm::AuthoredPlacementDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || localRow > snapshot.authoredPlacements.size()) {
        return false;
    }
    const catalog::AuthoredPlacement& row = snapshot.authoredPlacements[localRow - 1U];
    output.objectListName = selected_name(snapshot, row.objectListNameRow, true);
    output.classListName = selected_name(snapshot, row.classListNameRow, true);
    output.rotation = row.rotation;
    output.position = row.position;
    output.rotationBits = row.rotationBits;
    output.positionBits = row.positionBits;
    output.sourceOffset = row.sourceOffset;
    output.identifier = row.identifier;
    output.auxiliaryRelative = row.auxiliaryRelative;
    output.uniformScale = row.uniformScale;
    output.row = localRow;
    output.sourceObjectRow = one_based(row.sourceObjectRow);
    output.bubbleRow = one_based(row.bubbleRow);
    output.stateRow = one_based(row.stateRow);
    output.declaredBubbleIndex = row.declaredBubbleIndex;
    output.objectListTag = row.objectListTag;
    output.classListTag = row.classListTag;
    output.entryIndex = row.entryIndex;
    output.objectListNameRow = one_based(row.objectListNameRow);
    output.classListNameRow = one_based(row.classListNameRow);
    output.uniformScaleBits = row.uniformScaleBits;
    output.nameHash = row.nameHash;
    output.placementFlagsRaw = row.placementFlagsRaw;
    output.context = static_cast<std::uint32_t>(row.context);
    return true;
}

/** Copies one 1-based embedded placement row into its Lua-facing definition. */
[[nodiscard]] bool copy_embedded(const catalog::Snapshot& snapshot,
                                 std::uint32_t localRow,
                                 lua_vm::EmbeddedPlacementDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || localRow > snapshot.embeddedPlacements.size()) {
        return false;
    }
    const catalog::EmbeddedPlacement& row = snapshot.embeddedPlacements[localRow - 1U];
    output.classListName = selected_name(snapshot, row.classListNameRow, true);
    output.rotation = row.rotation;
    output.position = row.position;
    output.sourceOffset = row.sourceOffset;
    output.identifier = row.identifier;
    output.auxiliaryRelative = row.auxiliaryRelative;
    output.auxiliaryOffset = row.auxiliaryOffset;
    output.row = localRow;
    output.linkRow = one_based(row.linkRow);
    output.entryIndex = row.entryIndex;
    output.classListTag = row.classListTag;
    output.classListNameRow = one_based(row.classListNameRow);
    output.nameHash = row.nameHash;
    output.fourthLane = row.fourthLane;
    output.replicationByte = row.replicationByte;
    output.gameworldByte = row.gameworldByte;
    output.objectType = row.objectType;
    output.hasAuxiliary = row.hasAuxiliary;
    output.objectTypeRead = row.objectTypeRead;
    return true;
}

/** Copies one 1-based container placement row, joined through its placement list. */
[[nodiscard]] bool copy_container(const catalog::Snapshot& snapshot,
                                  std::uint32_t localRow,
                                  lua_vm::ContainerPlacementDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || localRow > snapshot.containerPlacements.size()) {
        return false;
    }
    const catalog::ContainerPlacement& row = snapshot.containerPlacements[localRow - 1U];
    if (row.listRow >= snapshot.containerPlacementLists.size()) {
        return false;
    }
    const catalog::ContainerPlacementList& list = snapshot.containerPlacementLists[row.listRow];
    output.objectListName = selected_name(snapshot, list.objectListNameRow, true);
    output.resourceName = selected_name(snapshot, list.resourceNameRow, true);
    output.classListName = selected_name(snapshot, row.classListNameRow, true);
    output.rotation = row.rotation;
    output.position = row.position;
    output.placementIdentifier = row.placementIdentifier;
    output.row = localRow;
    output.listRow = row.listRow + 1U;
    output.objectListTag = row.objectListTag;
    output.resourceTag = list.resourceTag;
    output.resourceClass = list.resourceClass;
    output.entryIndex = row.entryIndex;
    output.classListTag = row.classListTag;
    output.classListNameRow = one_based(row.classListNameRow);
    output.firstConfigRow = row.configCount == 0 ? 0U : row.firstConfig + 1U;
    output.configCount = row.configCount;
    output.uniformScale = row.uniformScale;
    output.objectType = row.objectType;
    output.resourceFieldRead = list.resourceFieldRead;
    output.resourceResolved = list.resourceResolved;
    output.listComplete = list.complete;
    output.placementIdentifierRead = row.placementIdentifierRead;
    output.complete = row.complete;
    return true;
}

/** Copies one 1-based static spatial instance, joined through its table row. */
[[nodiscard]] bool copy_static_spatial(const catalog::Snapshot& snapshot,
                                       std::uint32_t localRow,
                                       lua_vm::StaticSpatialDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || localRow > snapshot.staticSpatialInstances.size()) {
        return false;
    }
    const catalog::StaticSpatialInstance& row = snapshot.staticSpatialInstances[localRow - 1U];
    if (row.tableRow >= snapshot.staticSpatialTables.size()) {
        return false;
    }
    const catalog::StaticSpatialTable& table = snapshot.staticSpatialTables[row.tableRow];
    output.tableName = selected_name(snapshot, table.tableNameRow, true);
    output.boundsName = selected_name(snapshot, table.boundsNameRow, true);
    output.resourceName = selected_name(snapshot, row.resourceNameRow, true);
    output.rotation = row.rotationCandidate;
    output.position = row.positionCandidate;
    output.scale = row.scaleCandidate;
    output.localMinimum = row.localMinimum;
    output.localMaximum = row.localMaximum;
    output.boundsOpaque = row.boundsOpaque;
    output.row = localRow;
    output.tableRow = row.tableRow + 1U;
    output.instanceIndex = row.instanceIndex;
    output.tableTag = table.tableTag;
    output.boundsTag = table.boundsTag;
    output.resourceTag = row.resourceTag;
    output.resourceNameRow = one_based(row.resourceNameRow);
    output.tableComplete = table.complete;
    return true;
}

/** Copies one 1-based trigger volume instance, joined through its table row. */
[[nodiscard]] bool copy_trigger(const catalog::Snapshot& snapshot,
                                std::uint32_t localRow,
                                lua_vm::TriggerVolumeDefinition& output) noexcept {
    output = {};
    if (localRow == 0 || localRow > snapshot.triggerVolumeInstances.size()) {
        return false;
    }
    const catalog::TriggerVolumeInstance& row = snapshot.triggerVolumeInstances[localRow - 1U];
    if (row.tableRow >= snapshot.triggerVolumeTables.size()) {
        return false;
    }
    const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[row.tableRow];
    output.configName = selected_name(snapshot, table.configNameRow, true);
    output.classDefinitionName = selected_name(snapshot, row.classDefinitionNameRow, true);
    output.shapeResourceName = selected_name(snapshot, row.shapeResourceNameRow, true);
    output.rotation = row.rotation;
    output.position = row.position;
    output.minimum = row.minimum;
    output.maximum = row.maximum;
    output.row = localRow;
    output.tableRow = row.tableRow + 1U;
    output.authoredRowIndex = row.authoredRowIndex;
    output.configTag = table.configTag;
    output.identityMatchCount = table.identityMatchCount;
    output.registryKey = table.registryKey;
    output.componentOrdinal = table.componentOrdinal;
    output.slotIndex = table.slotIndex;
    output.slotType = table.slotType;
    output.classDefinitionTag = row.classDefinitionTag;
    output.classDefinitionNameRow = one_based(row.classDefinitionNameRow);
    output.shapeResourceTag = row.shapeResourceTag;
    output.shapeResourceNameRow = one_based(row.shapeResourceNameRow);
    output.shapeReferenceWord = row.shapeReferenceWord;
    output.shapeIndex = row.shapeIndex;
    output.firstVertexRow = row.vertexCount == 0 ? 0U : row.firstVertex + 1U;
    output.vertexCount = row.vertexCount;
    output.firstTriangleRow = row.triangleCount == 0 ? 0U : row.firstTriangle + 1U;
    output.triangleCount = row.triangleCount;
    output.flags = row.flags;
    output.extrusion = row.extrusion;
    output.active = row.active;
    output.tableComplete = table.complete;
    output.complete = row.complete;
    return true;
}

/** Resolves every non-positioned retained shard row without exposing storage addresses. */
[[nodiscard]] bool resolve_world(const void* context,
                                 const lua_vm::WorldGenerationIdentity& generation,
                                 lua_vm::WorldCollectionKind kind,
                                 std::uint32_t localRow,
                                 lua_vm::WorldRowDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr) {
        return false;
    }
    output.kind = kind;
    const catalog::Snapshot& snapshot = *world->snapshot();
    switch (kind) {
    case lua_vm::WorldCollectionKind::squadAnchors:
        return resolve_scenario_anchor(world->activity_sdk_view(), localRow, output.squadAnchor);
    case lua_vm::WorldCollectionKind::authoredPlacements:
        return copy_authored(snapshot, localRow, output.authoredPlacement);
    case lua_vm::WorldCollectionKind::embeddedPlacements:
        return copy_embedded(snapshot, localRow, output.embeddedPlacement);
    case lua_vm::WorldCollectionKind::containerPlacements:
        return copy_container(snapshot, localRow, output.containerPlacement);
    case lua_vm::WorldCollectionKind::staticSpatialInstances:
        return copy_static_spatial(snapshot, localRow, output.staticSpatial);
    case lua_vm::WorldCollectionKind::triggerVolumes:
        return copy_trigger(snapshot, localRow, output.triggerVolume);
    default:
        return localRow != 0 && localRow <= world_count_impl(context, generation, kind);
    }
}

/** @return Anchor count of one 1-based squad, or 0 when the generation or row is stale. */
[[nodiscard]] std::size_t squad_anchor_count(const void* context,
                                             const lua_vm::WorldGenerationIdentity& generation,
                                             std::uint32_t squadRow) noexcept {
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || squadRow == 0) {
        return 0;
    }
    const sdk::BoundView& view = world->activity_sdk_view();
    const auto squads = sdk::scenario_squads(*view.catalog, *sdk::bound_scenario(view));
    return squadRow <= squads.size()
               ? sdk::squad_anchors(*view.catalog, squads[squadRow - 1U]).size()
               : 0;
}

/** Resolves one 1-based anchor of one 1-based squad. */
[[nodiscard]] bool resolve_squad_anchor(const void* context,
                                        const lua_vm::WorldGenerationIdentity& generation,
                                        std::uint32_t squadRow,
                                        std::uint32_t localRow,
                                        lua_vm::SquadAnchorDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || squadRow == 0 || localRow == 0) {
        return false;
    }
    const sdk::BoundView& view = world->activity_sdk_view();
    const auto squads = sdk::scenario_squads(*view.catalog, *sdk::bound_scenario(view));
    if (squadRow > squads.size()) {
        return false;
    }
    const auto owned = sdk::squad_anchors(*view.catalog, squads[squadRow - 1U]);
    if (localRow > owned.size()) {
        return false;
    }
    std::uint32_t collectionRow = 0;
    for (std::uint32_t row = 1; row <= scenario_anchor_count(view); ++row) {
        lua_vm::SquadAnchorDefinition candidate{};
        if (!resolve_scenario_anchor(view, row, candidate)) {
            return false;
        }
        if (candidate.squadRow == squadRow
            && candidate.pointOrdinal == owned[localRow - 1U].pointOrdinal
            && candidate.row
                   == static_cast<std::uint32_t>(&owned[localRow - 1U]
                                                 - view.catalog->squad_anchors().data())
                          + 1U) {
            collectionRow = row;
            break;
        }
    }
    return collectionRow != 0
           && copy_anchor(
               view, squads[squadRow - 1U], owned[localRow - 1U], squadRow, collectionRow, output);
}

/** Resolves one 1-based vertex of one 1-based trigger volume. */
[[nodiscard]] bool resolve_trigger_vertex(const void* context,
                                          const lua_vm::WorldGenerationIdentity& generation,
                                          std::uint32_t triggerRow,
                                          std::uint32_t localRow,
                                          lua_vm::TriggerVertexDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || triggerRow == 0 || localRow == 0) {
        return false;
    }
    const catalog::Snapshot& snapshot = *world->snapshot();
    if (triggerRow > snapshot.triggerVolumeInstances.size()) {
        return false;
    }
    const catalog::TriggerVolumeInstance& trigger =
        snapshot.triggerVolumeInstances[triggerRow - 1U];
    if (localRow > trigger.vertexCount
        || trigger.firstVertex > snapshot.triggerVolumeVertices.size()
        || localRow - 1U > snapshot.triggerVolumeVertices.size() - trigger.firstVertex) {
        return false;
    }
    const std::uint32_t row = trigger.firstVertex + localRow - 1U;
    output.value = snapshot.triggerVolumeVertices[row].value;
    output.row = row + 1U;
    output.localRow = localRow;
    return true;
}

/** Resolves one 1-based triangle of one 1-based trigger volume. */
[[nodiscard]] bool resolve_trigger_triangle(const void* context,
                                            const lua_vm::WorldGenerationIdentity& generation,
                                            std::uint32_t triggerRow,
                                            std::uint32_t localRow,
                                            lua_vm::TriggerTriangleDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || triggerRow == 0 || localRow == 0) {
        return false;
    }
    const catalog::Snapshot& snapshot = *world->snapshot();
    if (triggerRow > snapshot.triggerVolumeInstances.size()) {
        return false;
    }
    const catalog::TriggerVolumeInstance& trigger =
        snapshot.triggerVolumeInstances[triggerRow - 1U];
    if (localRow > trigger.triangleCount
        || trigger.firstTriangle > snapshot.triggerVolumeTriangles.size()
        || localRow - 1U > snapshot.triggerVolumeTriangles.size() - trigger.firstTriangle) {
        return false;
    }
    const std::uint32_t row = trigger.firstTriangle + localRow - 1U;
    output.indices = snapshot.triggerVolumeTriangles[row].indices;
    output.row = row + 1U;
    output.localRow = localRow;
    return true;
}

/** Copies every diagnostic scalar without interpreting incomplete extraction as an empty set. */
[[nodiscard]] bool resolve_diagnostics(const void* context,
                                       const lua_vm::WorldGenerationIdentity& generation,
                                       lua_vm::WorldDiagnosticsDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr) {
        return false;
    }
    const catalog::Snapshot& row = *world->snapshot();
    output.detail = bounded_text(row.detail.data(), row.detail.size());
    output.revision = row.revision;
    output.request = row.request;
    output.unresolvedReads = row.unresolvedReads;
    const catalog::ContainerPlacementDiagnostics& container = row.containerPlacementDiagnostics;
    output.containerUnresolvedReads = container.unresolvedReads;
    output.containerSemanticUnresolved = container.semanticUnresolved;
    output.containerDroppedLists = container.droppedLists;
    output.containerDroppedOwners = container.droppedOwners;
    output.containerDroppedPlacements = container.droppedPlacements;
    output.containerDroppedConfigs = container.droppedConfigs;
    output.containerDroppedComponents = container.droppedComponents;
    output.containerContextResolved = container.contextResolved;
    output.containerContextNotApplicable = container.contextNotApplicable;
    output.containerIdentityOwnerInventoryComplete = container.identityOwnerInventoryComplete;
    output.containerComplete = container.complete;
    const catalog::EmbeddedPlacementDiagnostics& embedded = row.embeddedPlacementDiagnostics;
    output.embeddedApplicableDescriptors = embedded.applicableDescriptors;
    output.embeddedEmptyDescriptors = embedded.emptyDescriptors;
    output.embeddedReadPlacements = embedded.readPlacements;
    output.embeddedUnreadConfigurations = embedded.unreadConfigurations;
    output.embeddedMalformedDescriptors = embedded.malformedDescriptors;
    output.embeddedMalformedPlacements = embedded.malformedPlacements;
    output.embeddedUnresolvedClassDefinitions = embedded.unresolvedClassDefinitions;
    output.embeddedDroppedLinks = embedded.droppedLinks;
    output.embeddedDroppedPlacements = embedded.droppedPlacements;
    output.embeddedComplete = embedded.complete;
    const catalog::Type23PlacementDiagnostics& type23 = row.type23PlacementDiagnostics;
    output.type23UnreadIdentifiers = type23.unreadIdentifiers;
    output.type23DroppedLinks = type23.droppedLinks;
    output.type23DroppedCandidates = type23.droppedCandidates;
    output.type23ZeroIdentityMatches = type23.zeroIdentityMatches;
    output.type23MultipleIdentityMatches = type23.multipleIdentityMatches;
    output.type23ZeroActiveCandidates = type23.zeroActiveCandidates;
    output.type23MultipleActiveCandidates = type23.multipleActiveCandidates;
    output.type23Complete = type23.complete;
    output.staticSpatialUnresolvedReads = row.staticSpatialUnresolvedReads;
    output.staticSpatialSemanticUnresolved = row.staticSpatialSemanticUnresolved;
    output.staticSpatialDropped = row.staticSpatialDropped;
    output.staticSpatialContextResolved = row.staticSpatialContextResolved;
    output.staticSpatialNotApplicable = row.staticSpatialNotApplicable;
    output.staticSpatialComplete = row.staticSpatialComplete;
    const catalog::TriggerVolumeDiagnostics& trigger = row.triggerVolumeDiagnostics;
    output.triggerUnresolvedReads = trigger.unresolvedReads;
    output.triggerDroppedTables = trigger.droppedTables;
    output.triggerDroppedOwners = trigger.droppedOwners;
    output.triggerDroppedInstances = trigger.droppedInstances;
    output.triggerDroppedVertices = trigger.droppedVertices;
    output.triggerDroppedTriangles = trigger.droppedTriangles;
    output.triggerDroppedIncomingReferences = trigger.droppedIncomingReferences;
    output.triggerZeroMatches = trigger.zeroMatches;
    output.triggerMultipleMatches = trigger.multipleMatches;
    output.triggerComplete = trigger.complete;
    output.status = static_cast<std::uint32_t>(row.status);
    output.coverage = static_cast<std::uint32_t>(row.coverage);
    return true;
}

[[nodiscard]] std::size_t
coverage_count(const void* context, const lua_vm::WorldGenerationIdentity& generation) noexcept {
    return checked_world(context, generation) == nullptr ? 0U : catalog::kStructuralFamilyCount;
}

/** Resolves one 1-based structural family's coverage row. */
[[nodiscard]] bool resolve_coverage(const void* context,
                                    const lua_vm::WorldGenerationIdentity& generation,
                                    std::uint32_t localRow,
                                    lua_vm::WorldCoverageDefinition& output) noexcept {
    output = {};
    const generated::GeneratedWorldView* const world = checked_world(context, generation);
    if (world == nullptr || localRow == 0 || localRow > catalog::kStructuralFamilyCount) {
        return false;
    }
    const std::size_t index = localRow - 1U;
    const catalog::StructuralFamily family = static_cast<catalog::StructuralFamily>(index);
    const catalog::FamilyCoverageDiagnostic& source =
        world->snapshot()->coverageDiagnostics.families[index];
    output.family = catalog::structural_family_name(family);
    output.row = localRow;
    output.familyIndex = static_cast<std::uint32_t>(index);
    output.status = static_cast<std::uint32_t>(source.status);
    output.lossMask = source.lossMask;
    return output.family != "unknown";
}

} // namespace

std::size_t world_sdk_internal::world_count(const void* context,
                                            const lua_vm::WorldGenerationIdentity& generation,
                                            lua_vm::WorldCollectionKind kind) noexcept {
    return world_count_impl(context, generation, kind);
}

/** Builds the identity that pins a world view. @return False when the view is not fully bound. */
bool world_generation_identity(const generated::GeneratedWorldView& world,
                               lua_vm::WorldGenerationIdentity& output) noexcept {
    output = {};
    const sdk::BoundView& activity = world.activity_sdk_view();
    if (world.snapshot() == nullptr || activity.catalog == nullptr
        || sdk::bound_activity(activity) == nullptr || sdk::bound_scenario(activity) == nullptr
        || world.scenario_tag() == 0 || world.scenario_tag() != world.snapshot()->scenarioTag) {
        return false;
    }
    const generated::GenerationIdentity& source = world.generation_identity();
    output.sdkBuildSha256 = source.sdkBuildSha256;
    output.sdkPayloadSha256 = source.sdkPayloadSha256;
    output.sourceFingerprint = source.sourceFingerprint;
    output.manifestPayloadSha256 = source.manifestPayloadSha256;
    output.shardPayloadSha256 = source.shardPayloadSha256;
    output.activityClientGeneration = activity.activityClientGeneration;
    output.activityRow = activity.activityRow;
    output.scenarioTag = world.scenario_tag();
    return output.sdkBuildSha256 != std::array<std::byte, 32>{}
           && output.sdkPayloadSha256 != std::array<std::byte, 32>{}
           && output.sourceFingerprint != std::array<std::byte, 32>{}
           && output.manifestPayloadSha256 != std::array<std::byte, 32>{}
           && output.shardPayloadSha256 != std::array<std::byte, 32>{};
}

/** The byte layout is explicit so compiler padding cannot enter durable identity. */
bool world_program_generation_sha256(const generated::GeneratedWorldView& world,
                                     std::array<std::byte, 32>& output) noexcept {
    output = {};
    lua_vm::WorldGenerationIdentity generation{};
    if (!world_generation_identity(world, generation)) {
        return false;
    }
    std::array<std::byte, kProgramGenerationBytes> input{};
    std::size_t cursor = 0;
    std::memcpy(input.data(), kProgramGenerationDomain.data(), kProgramGenerationDomain.size());
    cursor += kProgramGenerationDomain.size() + 1U;
    const auto append = [&input, &cursor](const std::array<std::byte, 32>& value) noexcept {
        std::copy(value.begin(), value.end(), input.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += value.size();
    };
    append(generation.sourceFingerprint);
    append(generation.manifestPayloadSha256);
    append(generation.shardPayloadSha256);
    append(generation.sdkBuildSha256);
    append(generation.sdkPayloadSha256);
    for (std::size_t index = 0; index < kScenarioTagBytes; ++index) {
        input[cursor++] = static_cast<std::byte>(generation.scenarioTag >> (index * 8U));
    }
    return cursor == input.size() && crypto::hash(input, output);
}

/** Adds the world and manifest surfaces to the definition API, but only for a matching view. */
lua_vm::DefinitionApi definition_api(const sdk::BoundView& view,
                                     const generated::GeneratedWorldView& world) noexcept {
    lua_vm::DefinitionApi output = definition_api(view);
    lua_vm::WorldGenerationIdentity generation{};
    if (!world_generation_identity(world, generation)
        || world.activity_sdk_view().catalog != view.catalog
        || world.activity_sdk_view().activityRow != view.activityRow
        || world.activity_sdk_view().activityClientGeneration != view.activityClientGeneration) {
        return output;
    }
    output.manifest = manifest_definition_api(world);
    attach_actor_sequences(output, world);
    output.world = {
        .context = &world,
        .scenarioName = world.scenario_name(),
        .generation = generation,
        .validate = &validate_world,
        .count = &world_sdk_internal::world_count,
        .resolve = &resolve_world,
        .resolveField = &world_sdk_internal::resolve_world_field,
        .squadAnchorCount = &squad_anchor_count,
        .resolveSquadAnchor = &resolve_squad_anchor,
        .resolveTriggerVertex = &resolve_trigger_vertex,
        .resolveTriggerTriangle = &resolve_trigger_triangle,
        .resolveDiagnostics = &resolve_diagnostics,
        .coverageCount = &coverage_count,
        .resolveCoverage = &resolve_coverage,
    };
    return output;
}

} // namespace sunrise::server::activity::mission::sdk_bridge
