#include "native_targets.h"

namespace sunrise::izanami::research {

namespace {

constexpr std::array<NativeTargetRecord, static_cast<std::size_t>(NativeTargetId::count)>
    kNativeTargets{{
        {NativeTargetId::packageHeaderValidator,
         EvidenceStatus::mergedVerified,
         "package.accept.patch",
         "PR #26 trust path; keep package work behind adapter capabilities."},
        {NativeTargetId::loadedEntityPlacementInitialize,
         EvidenceStatus::openPrLead,
         "world.entity.spawn.loaded",
         "PR #32/#44 lead; validate on the supported build before use."},
        {NativeTargetId::loadedEntityDirectInitialize,
         EvidenceStatus::openPrLead,
         "world.entity.spawn.loaded",
         "Fallback initializer lead from PR #32/#44."},
        {NativeTargetId::objectFactory,
         EvidenceStatus::openPrLead,
         "world.entity.spawn.loaded",
         "Returns an ephemeral runtime datum; never serialize it."},
        {NativeTargetId::objectTransform,
         EvidenceStatus::openPrLead,
         "world.transform.write",
         "Candidate game-owned mutator; validate repeated edits and lifecycle."},
        {NativeTargetId::worldRaycast,
         EvidenceStatus::openPrLead,
         "physics.raycast.position",
         "Hit-position raycast lead; hit-object identity remains separate."},
        {NativeTargetId::objectDatumDescriptor,
         EvidenceStatus::openPrLead,
         "world.entity.spawn.loaded",
         "Datum resolution lead; build-specific descriptor must be validated."},
        {NativeTargetId::mapNodeConsumer,
         EvidenceStatus::historicalLead,
         "world.pattern.spawn",
         "Trace live SMapNodeEntry consumption before exposing pattern spawn."},
        {NativeTargetId::liveObjectRegistry,
         EvidenceStatus::historicalLead,
         "diagnostics.native_object_explorer",
         "Observability lead only; registry insertion is not a spawn API."},
        {NativeTargetId::havokSimulationStep,
         EvidenceStatus::currentBuildVerified,
         "physics.havok.world",
         "Current noclip traversal foothold; bind queries to the correct world."},
        {NativeTargetId::destinyDirectorMenuNode,
         EvidenceStatus::unknown,
         "destiny.director.node.inject",
         "Needed for a real Destiny Destinations node; no current native UI insertion point is "
         "validated."},
    }};

} // namespace

/** @return Research target metadata without executable signatures or addresses. */
std::span<const NativeTargetRecord> native_target_records() noexcept {
    return kNativeTargets;
}

} // namespace sunrise::izanami::research
