#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/named_tags.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/schema/generated/tag_layout_80808a_0.h"
#include "../../../middleware/content/packages/schema/generated/tag_layout_80809b_2.h"
#include "../../../middleware/content/packages/tables/activity_definition_reader.h"
#include "../../../state/activity_sdk/generated_world/catalog_manifest.h"

namespace sunrise::client::content::activity::sdk_generation::activity_inventory {

/** Tag class of one activity root that binds an investment activity name to a scenario. */
inline constexpr std::uint32_t kActivityRootClass =
    middleware::content::packages::schema::generated::ActivityRoot::kClass;
/** Tag class of the transition descriptor named by an activity root. */
inline constexpr std::uint32_t kActivityTransitionDescriptorClass =
    middleware::content::packages::schema::generated::ActivityTransitionDescriptor::kClass;
/** An activity root binds exactly two tags: row 0 the scenario, row 1 the transition. */
inline constexpr std::uint64_t kActivityRootBoundTagCount = 2U;
using state::activity_sdk::generated_world::manifest::kActivityRootRowsOffset;
using state::activity_sdk::generated_world::manifest::kActivityRootScenarioOffset;
using state::activity_sdk::generated_world::manifest::kActivityRootTransitionOffset;
/** An activity root ends with its last bound-tag row. */
inline constexpr std::size_t kActivityRootSize =
    kActivityRootRowsOffset
    + kActivityRootBoundTagCount
          * sizeof(middleware::content::packages::schema::generated::ActivityRootTagRow);
/** Transition descriptors are exact records of their class size. */
inline constexpr std::size_t kActivityTransitionDescriptorSize =
    middleware::content::packages::schema::generated::ActivityTransitionDescriptor::kSize;
/** A live root name is selected only when its effective named rows agree exactly. */
enum class NameStatus : std::uint8_t {
    exact,
    ambiguous,
    staleAliasesOnly,
    unnamed,
};

/** Exhaustive result of joining one investment activity's internal name to live roots. */
enum class JoinStatus : std::uint8_t {
    exact,
    liveNameMissing,
    sourceNameMissing,
    liveNameAmbiguous,
};

/** Exhaustive fixed-scenario applicability result for one activity definition. */
enum class BindingDisposition : std::uint8_t {
    fixedScenario = 0,
    namedDefinitionUnavailable = 1,
    noDirectFixedActivityName = 2,
    unresolvedRunnable = 3,
};

/** Stable reason for one binding disposition. */
enum class BindingReason : std::uint8_t {
    exactActivityRootScenarioEdge = 0,
    installedRouteAbsent = 1,
    noDirectFixedActivityName = 2,
    activityRootNameAmbiguous = 3,
    activityRootEdgeMissing = 4,
};

/** Source facts that support one binding classification. */
enum class BindingEvidenceBasis : std::uint8_t {
    effectiveActivityRootNamePlusPayloadScenarioEdge = 0,
    effectiveActivityAndScenarioRootNameCensus = 1,
    activityRecordInternalNameEmpty = 2,
    effectiveActivityRootNameCensus = 3,
};

/** Runtime interpretation retained without changing the fixed-scenario result. */
enum class RunnableStatus : std::uint8_t {
    fixedScenarioBound = 0,
    unavailableInInstalledEstate = 1,
    fixedScenarioNotApplicable = 2,
    unresolved = 3,
};

/** Whole-estate status derived from the exact four-way partition. */
enum class BindingCompletenessStatus : std::uint8_t {
    ready = 0,
    blockedUnresolvedRunnable = 1,
};

/** Addresses one payload: the tag names the package, the offset the byte inside it. */
struct PackageLocator final {
    std::uint32_t tag{};
    std::uint64_t offset{};

    bool operator==(const PackageLocator&) const = default;
};

/** Lossless source evidence for one activity binding classification. */
struct ActivityBindingEvidence final {
    bool hasInternalName{};
    bool hasMatchmakingConfig{};
    std::uint32_t matchmakingConfigTag{
        middleware::content::packages::tables::kActivityDefinitionNoMatchmakingConfig};
    std::vector<std::uint32_t> activityRootCandidateTags{};
    std::vector<std::uint32_t> scenarioNameCandidateTags{};
    std::vector<std::uint32_t> evidenceRootTags{};
    std::vector<PackageLocator> locators{};

    bool operator==(const ActivityBindingEvidence&) const = default;
};

/** Counted fixed-scenario applicability partition retained on one snapshot. */
struct BindingCompleteness final {
    std::size_t total{};
    std::size_t fixedScenario{};
    std::size_t namedDefinitionUnavailable{};
    std::size_t noDirectFixedActivityName{};
    std::size_t unresolvedRunnable{};
    BindingCompletenessStatus status{BindingCompletenessStatus::ready};

    bool operator==(const BindingCompleteness&) const = default;
};

/** One live scenario root and its exact selected package name. */
struct ScenarioRoot final {
    std::uint32_t tag{};
    std::array<char, middleware::content::packages::named_tags::kNameCapacity> name{};
    std::uint16_t nameLength{};
    NameStatus nameStatus{NameStatus::unnamed};
};

/** One live activity root with both exact package payload edges. */
struct ActivityRoot final {
    std::uint32_t tag{};
    std::uint32_t scenarioTag{};
    std::uint32_t transitionDescriptorTag{};
    std::array<char, middleware::content::packages::named_tags::kNameCapacity> name{};
    std::uint16_t nameLength{};
    NameStatus nameStatus{NameStatus::unnamed};
};

/** One investment activity and its optional exact root/scenario join. */
struct ActivityVariant final {
    middleware::content::packages::tables::ActivityDefinition definition{};
    std::uint32_t activityRootTag{};
    std::uint32_t scenarioTag{};
    JoinStatus joinStatus{JoinStatus::liveNameMissing};
    BindingDisposition bindingDisposition{BindingDisposition::unresolvedRunnable};
    BindingReason bindingReason{BindingReason::activityRootEdgeMissing};
    BindingEvidenceBasis bindingEvidenceBasis{
        BindingEvidenceBasis::effectiveActivityRootNameCensus};
    RunnableStatus runnableStatus{RunnableStatus::unresolved};
    bool fullSdkAcceptable{};
    ActivityBindingEvidence bindingEvidence{};
};

/** Measured join totals retained beside the inventory. */
struct Diagnostics final {
    std::size_t exact{};
    std::size_t liveNameMissing{};
    std::size_t sourceNameMissing{};
    std::size_t liveNameAmbiguous{};
};

/** Complete activity/root/scenario inventory derived from one installed content fingerprint. */
struct Snapshot final {
    std::vector<ScenarioRoot> scenarios{};
    std::vector<ActivityRoot> activityRoots{};
    std::vector<ActivityVariant> activities{};
    Diagnostics diagnostics{};
    BindingCompleteness bindingCompleteness{};
};

/** Optional cancellation probe used by the long package walk. */
using CancelProbe = bool (*)(void* context) noexcept;

/**
 * Joins already-validated activity definitions to exact selected activity-root names.
 * This pure step is public so synthetic tests and the generator share one policy.
 */
[[nodiscard]] bool
join(std::span<const middleware::content::packages::tables::ActivityDefinition> definitions,
     std::span<const ActivityRoot> roots,
     std::span<const ScenarioRoot> scenarios,
     std::vector<ActivityVariant>& activities,
     Diagnostics& diagnostics,
     BindingCompleteness& bindingCompleteness) noexcept;

/** Validates ordering, edge closure, and recomputed join/completeness totals. */
[[nodiscard]] bool validate(const Snapshot& snapshot) noexcept;

/**
 * Builds the complete native activity/root/scenario inventory from installed package data.
 * Package keys are borrowed only for authenticated tag reads and are never retained.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         CancelProbe cancel,
                         void* cancelContext,
                         Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::activity_inventory
