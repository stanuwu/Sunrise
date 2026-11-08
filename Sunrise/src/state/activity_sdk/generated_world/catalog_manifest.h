#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/reader/container_framing.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808084.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80808a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "codec.h"

namespace sunrise::state::activity_sdk::generated_world::manifest {

namespace generated = middleware::content::packages::schema::generated;

/** Version four persists the complete, independently verifiable activity-binding census. */
inline constexpr std::uint32_t kVersion = 4;
/** Missing activity joins use the package tag sentinel. */
inline constexpr std::uint32_t kAbsentTag = 0xFFFFFFFFU;
/** A catalog never accepts more scenario rows than the installed estate can need. */
inline constexpr std::size_t kMaximumScenarioRecords = 1024;
/** A catalog never accepts more activity roots than the installed estate can need. */
inline constexpr std::size_t kMaximumActivityRootRecords = 1024;
/** Activity variants include every definition, including unbound rows. */
inline constexpr std::size_t kMaximumActivityVariantRecords = 4096;
/** Existing scenario-only callers retain their bounded name. */
inline constexpr std::size_t kMaximumRecords = kMaximumScenarioRecords;
/** Activity and activity-root names use one fixed cache width. */
inline constexpr std::size_t kActivityNameCapacity = 128;
/** Bound-tag rows start after the root, its array marker and the 16-aligned array header. */
inline constexpr std::uint64_t kActivityRootRowsOffset =
    (sizeof(generated::ActivityRoot) + middleware::content::packages::reader::layout::kClassWordSize
     + generated::STypedArrayMarker::kAlign - 1U)
        / generated::STypedArrayMarker::kAlign * generated::STypedArrayMarker::kAlign
    + sizeof(generated::STypedArrayMarker);
/** Bound-tag row 0 names the scenario. */
inline constexpr std::uint64_t kActivityRootScenarioOffset =
    kActivityRootRowsOffset + offsetof(generated::ActivityRootTagRow, tag);
/** Bound-tag row 1 names the transition descriptor. */
inline constexpr std::uint64_t kActivityRootTransitionOffset =
    kActivityRootRowsOffset + sizeof(generated::ActivityRootTagRow)
    + offsetof(generated::ActivityRootTagRow, tag);

/** One generated scenario shard published by the catalog. */
struct ScenarioRecord final {
    std::uint32_t scenarioTag{};
    std::array<char, build_data::scriptables::kScenarioNameCapacity> scenarioName{};
    std::uint8_t scenarioNameLength{};
    Digest shardPayloadSha256{};
};

/** Existing scenario-only callers retain their public row name. */
using Record = ScenarioRecord;

/** Exact package-name selection state for one activity root. */
enum class ActivityRootSelectionStatus : std::uint8_t {
    exact = 0,
    ambiguous = 1,
    staleAliasesOnly = 2,
    unnamed = 3,
};

/** One activity root and its exact package payload edges. */
struct ActivityRootRecord final {
    std::uint32_t activityRootTag{};
    std::uint32_t scenarioTag{};
    std::uint32_t transitionDescriptorTag{};
    std::array<char, kActivityNameCapacity> preferredName{};
    std::uint8_t preferredNameLength{};
    ActivityRootSelectionStatus selectionStatus{};
};

/** Activity-to-root join result retained for every activity variant. */
enum class ActivityJoinStatus : std::uint8_t {
    exact = 0,
    liveNameMissing = 1,
    sourceNameMissing = 2,
    ambiguous = 3,
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

/** One exact package payload locator retained as classification evidence. */
struct PackageLocator final {
    std::uint32_t tag{};
    std::uint64_t offset{};

    [[nodiscard]] bool operator==(const PackageLocator&) const noexcept = default;
};

/** Counted fixed-scenario applicability partition retained on one catalog. */
struct BindingCompleteness final {
    std::size_t total{};
    std::size_t fixedScenario{};
    std::size_t namedDefinitionUnavailable{};
    std::size_t noDirectFixedActivityName{};
    std::size_t unresolvedRunnable{};
    BindingCompletenessStatus status{BindingCompletenessStatus::ready};

    [[nodiscard]] bool operator==(const BindingCompleteness&) const noexcept = default;
};

/** One activity definition, its binding result, and the complete evidence behind it. */
struct ActivityVariantRecord final {
    std::uint32_t activityIndex{};
    std::uint32_t definitionHash{};
    std::uint32_t activityRootTag{kAbsentTag};
    std::uint32_t scenarioTag{kAbsentTag};
    std::uint32_t matchmakingConfigTag{kAbsentTag};
    std::array<char, kActivityNameCapacity> internalName{};
    std::uint8_t internalNameLength{};
    ActivityJoinStatus joinStatus{};
    BindingDisposition bindingDisposition{BindingDisposition::unresolvedRunnable};
    BindingReason bindingReason{BindingReason::activityRootEdgeMissing};
    BindingEvidenceBasis bindingEvidenceBasis{
        BindingEvidenceBasis::effectiveActivityAndScenarioRootNameCensus};
    RunnableStatus runnableStatus{RunnableStatus::unresolved};
    bool fullSdkAcceptable{};
    bool hasInternalName{};
    bool hasMatchmakingConfig{};
    std::vector<std::uint32_t> activityRootCandidateTags{};
    std::vector<std::uint32_t> scenarioNameCandidateTags{};
    std::vector<std::uint32_t> evidenceRootTags{};
    std::vector<PackageLocator> locators{};
};

/** Exact runtime SDK generation that owns one generated-world catalog. */
struct SdkIdentity final {
    Digest buildSha256{};
    Digest payloadSha256{};

    [[nodiscard]] bool operator==(const SdkIdentity&) const noexcept = default;
};

/** One loaded catalog bound to the installed content that produced it. */
struct Catalog final {
    Digest sourceFingerprint{};
    SdkIdentity sdk{};
    Digest payloadSha256{};
    std::vector<ScenarioRecord> records;
    std::vector<ActivityRootRecord> activityRoots;
    std::vector<ActivityVariantRecord> activityVariants;
    BindingCompleteness bindingCompleteness{};
};

/** Result of loading one generated SDK catalog. */
enum class LoadStatus : std::uint8_t {
    loaded,
    missing,
    sourceMismatch,
    sdkMismatch,
    invalid,
    versionMismatch,
};

/** @return The stable log name of one catalog load result. */
[[nodiscard]] const char* status_name(LoadStatus value) noexcept;

/** Loads and validates one explicit generated SDK catalog path. */
[[nodiscard]] bool load(const wchar_t* path,
                        const Digest& expectedSourceFingerprint,
                        const SdkIdentity& expectedSdk,
                        Catalog& catalog,
                        LoadStatus& status) noexcept;

/**
 * Loads one source-authenticated catalog and returns its payload-owned SDK identity.
 * This boundary is used before opening the pack whose identity the catalog authorizes.
 */
[[nodiscard]] bool load(const wchar_t* path,
                        const Digest& expectedSourceFingerprint,
                        Catalog& catalog,
                        LoadStatus& status) noexcept;

/** Sorts, writes, reopens, and atomically publishes one explicit catalog path. */
[[nodiscard]] bool write(const wchar_t* path,
                         const Digest& sourceFingerprint,
                         const SdkIdentity& sdk,
                         std::span<const Record> records) noexcept;

/** Atomically publishes every scenario, root binding, and activity variant. */
[[nodiscard]] bool write(const wchar_t* path,
                         const Digest& sourceFingerprint,
                         const SdkIdentity& sdk,
                         std::span<const ScenarioRecord> scenarios,
                         std::span<const ActivityRootRecord> activityRoots,
                         std::span<const ActivityVariantRecord> activityVariants) noexcept;

} // namespace sunrise::state::activity_sdk::generated_world::manifest
