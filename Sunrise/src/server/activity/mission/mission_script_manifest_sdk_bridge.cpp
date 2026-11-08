#include "mission_script_manifest_sdk_bridge.h"

#include <algorithm>
#include <limits>

#include "mission_script_sdk_bridge.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {

namespace generated_world = state::activity_sdk::generated_world;
namespace manifest = state::activity_sdk::generated_world::manifest;

[[nodiscard]] const generated_world::GeneratedWorldView*
checked_view(const void* context, const lua_vm::WorldGenerationIdentity& expected) noexcept {
    const auto* const view = static_cast<const generated_world::GeneratedWorldView*>(context);
    lua_vm::WorldGenerationIdentity current{};
    return view != nullptr && view->manifest_catalog() != nullptr
                   && world_generation_identity(*view, current) && current == expected
               ? view
               : nullptr;
}

[[nodiscard]] bool validate_manifest(const void* context,
                                     const lua_vm::WorldGenerationIdentity& generation) noexcept {
    return checked_view(context, generation) != nullptr;
}

[[nodiscard]] bool field_u64(lua_vm::ManifestFieldDefinition& output,
                             std::uint64_t value) noexcept {
    output = {};
    output.kind = lua_vm::ManifestFieldKind::unsignedInteger;
    output.unsignedValue = value;
    return true;
}

[[nodiscard]] bool field_bool(lua_vm::ManifestFieldDefinition& output, bool value) noexcept {
    output = {};
    output.kind = lua_vm::ManifestFieldKind::boolean;
    output.unsignedValue = value ? 1U : 0U;
    return true;
}

[[nodiscard]] bool field_string(lua_vm::ManifestFieldDefinition& output,
                                std::string_view value) noexcept {
    output = {};
    output.kind = lua_vm::ManifestFieldKind::string;
    output.stringValue = value;
    return true;
}

[[nodiscard]] bool field_digest(lua_vm::ManifestFieldDefinition& output,
                                const generated_world::Digest& value) noexcept {
    output = {};
    output.kind = lua_vm::ManifestFieldKind::bytes;
    output.bytesValue = value;
    output.valueCount = static_cast<std::uint8_t>(value.size());
    return true;
}

/** @return The Lua-facing name of one activity-root selection status. */
[[nodiscard]] std::string_view
selection_name(manifest::ActivityRootSelectionStatus value) noexcept {
    switch (value) {
    case manifest::ActivityRootSelectionStatus::exact:
        return "exact";
    case manifest::ActivityRootSelectionStatus::ambiguous:
        return "ambiguous";
    case manifest::ActivityRootSelectionStatus::staleAliasesOnly:
        return "stale_aliases_only";
    case manifest::ActivityRootSelectionStatus::unnamed:
        return "unnamed";
    }
    return {};
}

/** @return The Lua-facing name of one activity join status. */
[[nodiscard]] std::string_view join_name(manifest::ActivityJoinStatus value) noexcept {
    switch (value) {
    case manifest::ActivityJoinStatus::exact:
        return "exact";
    case manifest::ActivityJoinStatus::liveNameMissing:
        return "live_name_missing";
    case manifest::ActivityJoinStatus::sourceNameMissing:
        return "source_name_missing";
    case manifest::ActivityJoinStatus::ambiguous:
        return "live_name_ambiguous";
    }
    return {};
}

/** @return The Lua-facing name of one binding disposition. */
[[nodiscard]] std::string_view disposition_name(manifest::BindingDisposition value) noexcept {
    switch (value) {
    case manifest::BindingDisposition::fixedScenario:
        return "fixed_scenario";
    case manifest::BindingDisposition::namedDefinitionUnavailable:
        return "named_definition_unavailable";
    case manifest::BindingDisposition::noDirectFixedActivityName:
        return "no_direct_fixed_activity_name";
    case manifest::BindingDisposition::unresolvedRunnable:
        return "unresolved_runnable";
    }
    return {};
}

/** @return The Lua-facing name of one binding reason. */
[[nodiscard]] std::string_view reason_name(manifest::BindingReason value) noexcept {
    switch (value) {
    case manifest::BindingReason::exactActivityRootScenarioEdge:
        return "exact_activity_root_scenario_edge";
    case manifest::BindingReason::installedRouteAbsent:
        return "installed_route_absent";
    case manifest::BindingReason::noDirectFixedActivityName:
        return "no_direct_fixed_activity_name";
    case manifest::BindingReason::activityRootNameAmbiguous:
        return "activity_root_name_ambiguous";
    case manifest::BindingReason::activityRootEdgeMissing:
        return "activity_root_edge_missing";
    }
    return {};
}

/** @return The Lua-facing name of one binding evidence basis. */
[[nodiscard]] std::string_view evidence_name(manifest::BindingEvidenceBasis value) noexcept {
    switch (value) {
    case manifest::BindingEvidenceBasis::effectiveActivityRootNamePlusPayloadScenarioEdge:
        return "effective_activity_root_name_plus_payload_scenario_edge";
    case manifest::BindingEvidenceBasis::effectiveActivityAndScenarioRootNameCensus:
        return "effective_activity_and_scenario_root_name_census";
    case manifest::BindingEvidenceBasis::activityRecordInternalNameEmpty:
        return "activity_record_internal_name_empty";
    case manifest::BindingEvidenceBasis::effectiveActivityRootNameCensus:
        return "effective_activity_root_name_census";
    }
    return {};
}

/** @return The Lua-facing name of one runnable status. */
[[nodiscard]] std::string_view runnable_name(manifest::RunnableStatus value) noexcept {
    switch (value) {
    case manifest::RunnableStatus::fixedScenarioBound:
        return "fixed_scenario_bound";
    case manifest::RunnableStatus::unavailableInInstalledEstate:
        return "unavailable_in_installed_estate";
    case manifest::RunnableStatus::fixedScenarioNotApplicable:
        return "fixed_scenario_not_applicable";
    case manifest::RunnableStatus::unresolved:
        return "unresolved";
    }
    return {};
}

[[nodiscard]] std::string_view
completeness_name(manifest::BindingCompletenessStatus value) noexcept {
    switch (value) {
    case manifest::BindingCompletenessStatus::ready:
        return "ready";
    case manifest::BindingCompletenessStatus::blockedUnresolvedRunnable:
        return "blocked_unresolved_runnable";
    }
    return {};
}

/** @return Row count of one manifest collection kind, or 0 when the generation is stale. */
[[nodiscard]] std::size_t manifest_count(const void* context,
                                         const lua_vm::WorldGenerationIdentity& generation,
                                         lua_vm::ManifestCollectionKind kind) noexcept {
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    if (view == nullptr) {
        return 0;
    }
    const manifest::Catalog& catalog = *view->manifest_catalog();
    switch (kind) {
    case lua_vm::ManifestCollectionKind::scenarios:
        return catalog.records.size();
    case lua_vm::ManifestCollectionKind::activityRoots:
        return catalog.activityRoots.size();
    case lua_vm::ManifestCollectionKind::activityVariants:
        return catalog.activityVariants.size();
    case lua_vm::ManifestCollectionKind::bindingCompleteness:
        return 1;
    }
    return 0;
}

/** Reads one named field of a scenario record; an unknown key reads absent, `row` is refused. */
[[nodiscard]] bool scenario_field(const manifest::ScenarioRecord& row,
                                  std::string_view key,
                                  lua_vm::ManifestFieldDefinition& output) noexcept {
    if (key == "row") {
        return false;
    }
    if (key == "scenario_tag") {
        return field_u64(output, row.scenarioTag);
    }
    if (key == "scenario_name") {
        return field_string(output, {row.scenarioName.data(), row.scenarioNameLength});
    }
    if (key == "shard_payload_sha256") {
        return field_digest(output, row.shardPayloadSha256);
    }
    output = {};
    return true;
}

/** Reads one named field of an activity root record; an unknown key reads absent. */
[[nodiscard]] bool root_field(const manifest::ActivityRootRecord& row,
                              std::string_view key,
                              lua_vm::ManifestFieldDefinition& output) noexcept {
    if (key == "activity_root_tag") {
        return field_u64(output, row.activityRootTag);
    }
    if (key == "scenario_tag") {
        return field_u64(output, row.scenarioTag);
    }
    if (key == "transition_descriptor_tag") {
        return field_u64(output, row.transitionDescriptorTag);
    }
    if (key == "preferred_name") {
        return field_string(output, {row.preferredName.data(), row.preferredNameLength});
    }
    if (key == "selection_status") {
        return field_u64(output, static_cast<std::uint8_t>(row.selectionStatus));
    }
    if (key == "selection_status_name") {
        return field_string(output, selection_name(row.selectionStatus));
    }
    output = {};
    return true;
}

template <typename Selector>
[[nodiscard]] std::uint64_t preceding_count(std::span<const manifest::ActivityVariantRecord> rows,
                                            std::size_t row,
                                            Selector select) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < row; ++index) {
        result += select(rows[index]).size();
    }
    return result;
}

/** @return The flat index of one variant tag kind's first tag, counted across all earlier rows. */
[[nodiscard]] std::uint64_t
evidence_tag_first(std::span<const manifest::ActivityVariantRecord> rows,
                   std::size_t row,
                   lua_vm::ManifestVariantTagKind kind) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < row; ++index) {
        result += rows[index].activityRootCandidateTags.size();
        result += rows[index].scenarioNameCandidateTags.size();
        result += rows[index].evidenceRootTags.size();
    }
    if (kind != lua_vm::ManifestVariantTagKind::activityRootCandidates) {
        result += rows[row].activityRootCandidateTags.size();
    }
    if (kind == lua_vm::ManifestVariantTagKind::evidenceRoots) {
        result += rows[row].scenarioNameCandidateTags.size();
    }
    return result;
}

/** Reads one named field of an activity variant row; an unknown key reads absent. */
[[nodiscard]] bool variant_field(std::span<const manifest::ActivityVariantRecord> rows,
                                 std::size_t index,
                                 std::string_view key,
                                 lua_vm::ManifestFieldDefinition& output) noexcept {
    const manifest::ActivityVariantRecord& row = rows[index];
#define SUNRISE_MANIFEST_U32(NAME, MEMBER)                                                         \
    if (key == (NAME)) {                                                                           \
        return field_u64(output, row.MEMBER);                                                      \
    }
    SUNRISE_MANIFEST_U32("activity_index", activityIndex)
    SUNRISE_MANIFEST_U32("definition_hash", definitionHash)
    SUNRISE_MANIFEST_U32("activity_root_tag", activityRootTag)
    SUNRISE_MANIFEST_U32("scenario_tag", scenarioTag)
    SUNRISE_MANIFEST_U32("matchmaking_config_tag", matchmakingConfigTag)
#undef SUNRISE_MANIFEST_U32
    if (key == "internal_name") {
        return field_string(output, {row.internalName.data(), row.internalNameLength});
    }
#define SUNRISE_MANIFEST_ENUM(NAME, MEMBER, NAMER)                                                 \
    if (key == (NAME)) {                                                                           \
        return field_u64(output, static_cast<std::uint8_t>(row.MEMBER));                           \
    }                                                                                              \
    if (key == NAME "_name") {                                                                     \
        return field_string(output, NAMER(row.MEMBER));                                            \
    }
    SUNRISE_MANIFEST_ENUM("join_status", joinStatus, join_name)
    SUNRISE_MANIFEST_ENUM("binding_disposition", bindingDisposition, disposition_name)
    SUNRISE_MANIFEST_ENUM("binding_reason", bindingReason, reason_name)
    SUNRISE_MANIFEST_ENUM("binding_evidence_basis", bindingEvidenceBasis, evidence_name)
    SUNRISE_MANIFEST_ENUM("runnable_status", runnableStatus, runnable_name)
#undef SUNRISE_MANIFEST_ENUM
#define SUNRISE_MANIFEST_BOOL(NAME, MEMBER)                                                        \
    if (key == (NAME)) {                                                                           \
        return field_bool(output, row.MEMBER);                                                     \
    }
    SUNRISE_MANIFEST_BOOL("full_sdk_acceptable", fullSdkAcceptable)
    SUNRISE_MANIFEST_BOOL("has_internal_name", hasInternalName)
    SUNRISE_MANIFEST_BOOL("has_matchmaking_config", hasMatchmakingConfig)
#undef SUNRISE_MANIFEST_BOOL
#define SUNRISE_MANIFEST_TAG_RANGE(NAME, MEMBER, KIND)                                             \
    if (key == NAME "_first_index") {                                                              \
        return field_u64(output, evidence_tag_first(rows, index, KIND));                           \
    }                                                                                              \
    if (key == NAME "_count") {                                                                    \
        return field_u64(output, row.MEMBER.size());                                               \
    }
    SUNRISE_MANIFEST_TAG_RANGE("activity_root_candidate_tags",
                               activityRootCandidateTags,
                               lua_vm::ManifestVariantTagKind::activityRootCandidates)
    SUNRISE_MANIFEST_TAG_RANGE("scenario_name_candidate_tags",
                               scenarioNameCandidateTags,
                               lua_vm::ManifestVariantTagKind::scenarioNameCandidates)
    SUNRISE_MANIFEST_TAG_RANGE(
        "evidence_root_tags", evidenceRootTags, lua_vm::ManifestVariantTagKind::evidenceRoots)
#undef SUNRISE_MANIFEST_TAG_RANGE
    if (key == "binding_locators_first_index") {
        return field_u64(output, preceding_count(rows, index, [](const auto& value) -> const auto& {
                             return value.locators;
                         }));
    }
    if (key == "binding_locators_count") {
        return field_u64(output, row.locators.size());
    }
    output = {};
    return true;
}

/** Reads one named field of a binding-completeness row; an unknown key reads absent. */
[[nodiscard]] bool completeness_field(const manifest::BindingCompleteness& row,
                                      std::string_view key,
                                      lua_vm::ManifestFieldDefinition& output) noexcept {
#define SUNRISE_MANIFEST_COUNT(NAME, MEMBER)                                                       \
    if (key == (NAME)) {                                                                           \
        return field_u64(output, row.MEMBER);                                                      \
    }
    SUNRISE_MANIFEST_COUNT("total", total)
    SUNRISE_MANIFEST_COUNT("fixed_scenario", fixedScenario)
    SUNRISE_MANIFEST_COUNT("named_definition_unavailable", namedDefinitionUnavailable)
    SUNRISE_MANIFEST_COUNT("no_direct_fixed_activity_name", noDirectFixedActivityName)
    SUNRISE_MANIFEST_COUNT("unresolved_runnable", unresolvedRunnable)
#undef SUNRISE_MANIFEST_COUNT
    if (key == "status") {
        return field_u64(output, static_cast<std::uint8_t>(row.status));
    }
    if (key == "status_name") {
        return field_string(output, completeness_name(row.status));
    }
    output = {};
    return true;
}

/** Resolves one named manifest field against the collection kind and row the handle names. */
[[nodiscard]] bool resolve_manifest_field(const void* context,
                                          const lua_vm::WorldGenerationIdentity& generation,
                                          lua_vm::ManifestCollectionKind kind,
                                          std::uint32_t row,
                                          std::string_view key,
                                          lua_vm::ManifestFieldDefinition& output) noexcept {
    output = {};
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    if (view == nullptr) {
        return false;
    }
    const manifest::Catalog& catalog = *view->manifest_catalog();
    switch (kind) {
    case lua_vm::ManifestCollectionKind::scenarios:
        return row < catalog.records.size() && scenario_field(catalog.records[row], key, output);
    case lua_vm::ManifestCollectionKind::activityRoots:
        return row < catalog.activityRoots.size()
               && root_field(catalog.activityRoots[row], key, output);
    case lua_vm::ManifestCollectionKind::activityVariants:
        return row < catalog.activityVariants.size()
               && variant_field(catalog.activityVariants, row, key, output);
    case lua_vm::ManifestCollectionKind::bindingCompleteness:
        return row == 0 && completeness_field(catalog.bindingCompleteness, key, output);
    }
    return false;
}

/** @return The tag span one variant kind names, or empty when the kind is unknown. */
[[nodiscard]] std::span<const std::uint32_t>
variant_tags(const manifest::ActivityVariantRecord& row,
             lua_vm::ManifestVariantTagKind kind) noexcept {
    switch (kind) {
    case lua_vm::ManifestVariantTagKind::activityRootCandidates:
        return row.activityRootCandidateTags;
    case lua_vm::ManifestVariantTagKind::scenarioNameCandidates:
        return row.scenarioNameCandidateTags;
    case lua_vm::ManifestVariantTagKind::evidenceRoots:
        return row.evidenceRootTags;
    }
    return {};
}

[[nodiscard]] std::size_t variant_tag_count(const void* context,
                                            const lua_vm::WorldGenerationIdentity& generation,
                                            std::uint32_t row,
                                            lua_vm::ManifestVariantTagKind kind) noexcept {
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    const manifest::Catalog* const catalog = view != nullptr ? view->manifest_catalog() : nullptr;
    return catalog != nullptr && row < catalog->activityVariants.size()
               ? variant_tags(catalog->activityVariants[row], kind).size()
               : 0;
}

/** Resolves one tag of one activity variant row; both rows are zero-based. */
[[nodiscard]] bool resolve_variant_tag(const void* context,
                                       const lua_vm::WorldGenerationIdentity& generation,
                                       std::uint32_t row,
                                       lua_vm::ManifestVariantTagKind kind,
                                       std::uint32_t localRow,
                                       std::uint32_t& output) noexcept {
    output = 0;
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    const manifest::Catalog* const catalog = view != nullptr ? view->manifest_catalog() : nullptr;
    if (catalog == nullptr || row >= catalog->activityVariants.size()) {
        return false;
    }
    const auto values = variant_tags(catalog->activityVariants[row], kind);
    if (localRow >= values.size()) {
        return false;
    }
    output = values[localRow];
    return true;
}

[[nodiscard]] std::size_t variant_locator_count(const void* context,
                                                const lua_vm::WorldGenerationIdentity& generation,
                                                std::uint32_t row) noexcept {
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    const manifest::Catalog* const catalog = view != nullptr ? view->manifest_catalog() : nullptr;
    return catalog != nullptr && row < catalog->activityVariants.size()
               ? catalog->activityVariants[row].locators.size()
               : 0;
}

/** Resolves one payload locator of one activity variant row; both rows are zero-based. */
[[nodiscard]] bool resolve_variant_locator(const void* context,
                                           const lua_vm::WorldGenerationIdentity& generation,
                                           std::uint32_t row,
                                           std::uint32_t localRow,
                                           lua_vm::ManifestLocatorDefinition& output) noexcept {
    output = {};
    const generated_world::GeneratedWorldView* const view = checked_view(context, generation);
    const manifest::Catalog* const catalog = view != nullptr ? view->manifest_catalog() : nullptr;
    if (catalog == nullptr || row >= catalog->activityVariants.size()
        || localRow >= catalog->activityVariants[row].locators.size()) {
        return false;
    }
    const manifest::PackageLocator& locator = catalog->activityVariants[row].locators[localRow];
    output.tag = locator.tag;
    output.offset = locator.offset;
    output.localRow = localRow + 1U;
    return true;
}

} // namespace

/** @return The Lua manifest reader bound to this world, or an empty api when none is bound. */
lua_vm::ManifestDefinitionApi
manifest_definition_api(const generated_world::GeneratedWorldView& world) noexcept {
    lua_vm::WorldGenerationIdentity generation{};
    if (world.manifest_catalog() == nullptr || !world_generation_identity(world, generation)) {
        return {};
    }
    return {
        .context = &world,
        .generation = generation,
        .validate = &validate_manifest,
        .count = &manifest_count,
        .resolveField = &resolve_manifest_field,
        .variantTagCount = &variant_tag_count,
        .resolveVariantTag = &resolve_variant_tag,
        .variantLocatorCount = &variant_locator_count,
        .resolveVariantLocator = &resolve_variant_locator,
    };
}

} // namespace sunrise::server::activity::mission::sdk_bridge
