#include "scriptable_catalog_container_placements.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../state/build_data/runtime.h"
#include "../scenarios/scenario_build.h"
#include "../spawn_sets/spawn_set_catalog_builder.h"
#include "scriptable_catalog_container_placements_internal.h"
#include "scriptable_catalog_inline_names.h"

namespace sunrise::client::content::activity::scriptables::internal {

namespace {

using namespace container_placements;

namespace catalog = state::build_data::scriptables;
namespace package_reader = middleware::content::packages::reader;
namespace scenario_layouts = ::sunrise::client::content::scenarios;
namespace tables = middleware::content::packages::tables;

/** Reads one reached tag and keeps all valid inline-name evidence in its blob. */
[[nodiscard]] bool read_tag(BuildContext& context,
                            std::uint32_t tag,
                            std::vector<std::byte>& output,
                            std::uint32_t& classId) noexcept {
    if (!package_reader::read_tag(*context.source, *context.scratch, tag, output, classId)) {
        return false;
    }
    if (!collect_inline_name_evidence(*context.output, output)) {
        context.failed = true;
        return false;
    }
    return true;
}

/** Marks one recoverable package read or layout failure. */
void mark_unresolved(BuildContext& context) noexcept {
    ++context.output->containerPlacementDiagnostics.unresolvedReads;
    context.output->containerPlacementDiagnostics.complete = false;
}

/** Records a lossless raw edge whose target meaning remains unresolved. */
// TODO: no raw-edge path calls this yet, so semanticUnresolved always publishes zero.
void mark_semantic_unresolved(BuildContext& context) noexcept {
    ++context.output->containerPlacementDiagnostics.semanticUnresolved;
}

/** Marks loss that can hide a placement identity or one of its applicable owners. */
void mark_identity_owner_incomplete(BuildContext& context) noexcept {
    context.output->containerPlacementDiagnostics.identityOwnerInventoryComplete = false;
    context.output->containerPlacementDiagnostics.complete = false;
}

/** Marks one unresolved identity/owner input while retaining the general read diagnostic. */
void mark_identity_owner_unresolved(BuildContext& context) noexcept {
    mark_unresolved(context);
    mark_identity_owner_incomplete(context);
}

/** Adds a bounded loss count without allowing the diagnostic to wrap. */
void add_dropped(std::uint64_t& destination, std::size_t count) noexcept {
    const std::uint64_t available = (std::numeric_limits<std::uint64_t>::max)() - destination;
    destination += (std::min)(available, static_cast<std::uint64_t>(count));
}

/** @return True when one swept index entry belongs to the selected scenario stem. */
[[nodiscard]] bool same_stem(const BuildContext& context,
                             const ContainerIndexEntry& entry) noexcept {
    return entry.stemValid && std::string_view(entry.stem.data(), entry.stemLength) == context.stem;
}

/** @return Scenario bubble rows selected by one map-global authored mask. */
[[nodiscard]] std::uint64_t scenario_bubble_mask(
    const catalog::Snapshot& output,
    const std::array<std::uint8_t, tables::kContainerBubbleMaskBytes>& mapMask) noexcept {
    std::uint64_t result = 0;
    for (const catalog::State& state : output.states) {
        if (state.bubbleRow < std::numeric_limits<std::uint64_t>::digits
            && tables::bubble_in_mask(mapMask, state.mapBubbleIndex)) {
            result |= std::uint64_t{1} << state.bubbleRow;
        }
    }
    return result;
}

/** @return True when every transform lane is safe for distance and render math. */
template <std::size_t Size>
[[nodiscard]] bool finite(const std::array<float, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float lane) { return std::isfinite(lane); });
}

/** Reads one tag only when its class matches the typed edge being followed. */
[[nodiscard]] bool read_exact(BuildContext& context,
                              std::uint32_t tag,
                              std::uint32_t expectedClass,
                              std::vector<std::byte>& output) noexcept {
    std::uint32_t classId = 0;
    if (cancelled(context)) {
        context.failed = true;
        return false;
    }
    if (!read_tag(context, tag, output, classId) || classId != expectedClass) {
        mark_unresolved(context);
        return false;
    }
    return true;
}

/** Appends the exact typed component rows of one placed config. */
[[nodiscard]] bool
append_components(BuildContext& context, std::uint32_t configRow, std::span<const std::byte> blob) {
    catalog::ContainerPlacementConfig& config =
        context.output->containerPlacementConfigs[configRow];
    config.firstComponent =
        static_cast<std::uint32_t>(context.output->containerPlacementComponents.size());
    tables::Array components{};
    if (!tables::placed_config_components(blob, components)) {
        mark_unresolved(context);
        return false;
    }
    const std::size_t retained =
        (std::min)(static_cast<std::size_t>(components.count),
                   kComponentCapacity - context.output->containerPlacementComponents.size());
    for (std::size_t index = 0; index < retained; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::PlacedConfigComponentRow source{};
        if (!tables::placed_config_component_at(blob, components, index, source)) {
            mark_unresolved(context);
            return false;
        }
        context.output->containerPlacementComponents.push_back({configRow,
                                                                source.componentClass,
                                                                source.firstWord,
                                                                source.secondWord,
                                                                source.fourthWord,
                                                                static_cast<std::uint32_t>(index)});
    }
    config.componentCount = static_cast<std::uint32_t>(retained);
    if (retained != components.count) {
        add_dropped(context.output->containerPlacementDiagnostics.droppedComponents,
                    static_cast<std::size_t>(components.count) - retained);
        context.output->containerPlacementDiagnostics.complete = false;
        return false;
    }
    return true;
}

/** Appends one config occurrence and its exact component edges. */
[[nodiscard]] bool append_config(BuildContext& context,
                                 std::uint32_t placementRow,
                                 std::uint32_t buildOrdinal,
                                 const tables::PlacedClassBuildRow& source) {
    if (context.output->containerPlacementConfigs.size() >= kConfigCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedConfigs;
        context.output->containerPlacementDiagnostics.complete = false;
        return false;
    }
    context.output->containerPlacementConfigs.emplace_back();
    catalog::ContainerPlacementConfig& destination =
        context.output->containerPlacementConfigs.back();
    zero_row_storage(destination);
    destination.placementRow = placementRow;
    destination.configTag = source.configTag;
    destination.configNameRow = catalog::kNoRow;
    destination.buildOrdinal = buildOrdinal;
    destination.secondWord = source.secondWord;
    destination.thirdWord = source.thirdWord;
    const std::uint32_t row =
        static_cast<std::uint32_t>(context.output->containerPlacementConfigs.size() - 1);
    bool complete =
        read_exact(context, source.configTag, tables::kPlacedConfigClass, context.configBytes);
    if (complete) {
        complete = append_components(context, row, context.configBytes);
    }
    context.output->containerPlacementConfigs[row].complete = complete && !context.failed;
    return complete;
}

/** Appends the ordered config graph of one exact placed class definition. */
[[nodiscard]] bool
append_class_graph(BuildContext& context, std::uint32_t placementRow, std::uint32_t classTag) {
    catalog::ContainerPlacement& placement = context.output->containerPlacements[placementRow];
    if (!read_exact(context, classTag, tables::kPlacedClassDefinitionClass, context.classBytes)) {
        return false;
    }
    tables::PlacedClassDefinition source{};
    if (!tables::placed_class_definition(context.classBytes, source)) {
        mark_unresolved(context);
        return false;
    }
    placement.objectType = source.objectType;
    placement.firstConfig =
        static_cast<std::uint32_t>(context.output->containerPlacementConfigs.size());
    bool complete = true;
    for (std::size_t index = 0; index < source.builds.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::PlacedClassBuildRow build{};
        if (!tables::placed_class_build_at(context.classBytes, source.builds, index, build)) {
            mark_unresolved(context);
            complete = false;
            break;
        }
        if (!append_config(context, placementRow, static_cast<std::uint32_t>(index), build)) {
            complete = false;
            if (context.output->containerPlacementConfigs.size() >= kConfigCapacity) {
                add_dropped(context.output->containerPlacementDiagnostics.droppedConfigs,
                            static_cast<std::size_t>(source.builds.count) - index - 1);
                break;
            }
        }
    }
    placement.configCount = static_cast<std::uint32_t>(
        context.output->containerPlacementConfigs.size() - placement.firstConfig);
    return complete && placement.configCount == source.builds.count;
}

/** Appends every exact transform from one unique object list. */
[[nodiscard]] bool
append_placements(BuildContext& context, std::uint32_t listRow, std::uint32_t objectListTag) {
    tables::Array placements{};
    if (!tables::authored_placements(context.listBytes, placements)) {
        mark_identity_owner_unresolved(context);
        return false;
    }
    const std::size_t retained =
        (std::min)(static_cast<std::size_t>(placements.count),
                   kPlacementCapacity - context.output->containerPlacements.size());
    bool complete = retained == placements.count;
    if (!complete) {
        add_dropped(context.output->containerPlacementDiagnostics.droppedPlacements,
                    static_cast<std::size_t>(placements.count) - retained);
        mark_identity_owner_incomplete(context);
    }
    for (std::size_t index = 0; index < retained; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        tables::AuthoredPlacement source{};
        float uniformScale = 0.0F;
        std::uint64_t placementIdentifier = 0;
        if (!tables::authored_placement_at(context.listBytes, placements, index, source)
            || !tables::authored_placement_identifier_at(
                context.listBytes, placements, index, placementIdentifier)
            || !tables::container_placement_uniform_scale_at(
                context.listBytes, placements, index, uniformScale)
            || !finite(source.rotation) || !finite(source.position)
            || !std::isfinite(uniformScale)) {
            ++context.output->containerPlacementDiagnostics.droppedPlacements;
            mark_identity_owner_unresolved(context);
            complete = false;
            continue;
        }
        context.output->containerPlacements.emplace_back();
        catalog::ContainerPlacement& destination = context.output->containerPlacements.back();
        zero_row_storage(destination);
        destination.listRow = listRow;
        destination.objectListTag = objectListTag;
        destination.entryIndex = static_cast<std::uint32_t>(index);
        destination.classListTag = source.classListTag;
        destination.classListNameRow = catalog::kNoRow;
        destination.rotation = source.rotation;
        destination.position = source.position;
        destination.uniformScale = uniformScale;
        destination.placementIdentifier = placementIdentifier;
        destination.placementIdentifierRead = true;
        const std::uint32_t row =
            static_cast<std::uint32_t>(context.output->containerPlacements.size() - 1);
        const bool rowComplete = append_class_graph(context, row, source.classListTag);
        context.output->containerPlacements[row].complete = rowComplete;
        complete = complete && rowComplete;
    }
    return complete;
}

/** Reads the list-level resource edge and every placement below one unique list. */
void materialize_list(BuildContext& context, std::uint32_t listRow) {
    catalog::ContainerPlacementList& list = context.output->containerPlacementLists[listRow];
    // TODO: fill the list's resource fields once the component member shape is read; nothing
    // reads them yet, and `component_resource` reads a component field, not a list field.
    if (context.cache != nullptr) {
        const auto cached = ContainerPlacementCacheAccess::find(*context.cache, list.objectListTag);
        if (cached != nullptr) {
            const CacheReplay replay = replay_cached_graph(context, listRow, *cached);
            if (replay == CacheReplay::complete) {
                list.complete = true;
                return;
            }
            if (replay == CacheReplay::cancelled) {
                context.failed = true;
                list.complete = false;
                return;
            }
        }
    }
    const std::size_t firstPlacement = context.output->containerPlacements.size();
    const std::size_t firstConfig = context.output->containerPlacementConfigs.size();
    const std::size_t firstComponent = context.output->containerPlacementComponents.size();
    const std::size_t firstInlineName = context.output->inlineNameCandidates.size();
    const std::size_t firstInlineByte = context.output->inlineNameBytes.size();
    list.complete = append_placements(context, listRow, list.objectListTag);
    if (list.complete && !context.failed && !cancelled(context) && context.cache != nullptr) {
        ContainerPlacementCacheAccess::Value cached{};
        if (capture_cached_graph(*context.output,
                                 listRow,
                                 firstPlacement,
                                 firstConfig,
                                 firstComponent,
                                 firstInlineName,
                                 firstInlineByte,
                                 cached)) {
            ContainerPlacementCacheAccess::remember(
                *context.cache, list.objectListTag, std::move(cached));
        }
    }
}

/** @return Existing list row, or a new row after its graph is materialized. */
[[nodiscard]] std::uint32_t ensure_list(BuildContext& context, std::uint32_t tag) {
    const auto found = context.listRows.find(tag);
    if (found != context.listRows.end()) {
        return found->second;
    }
    if (context.output->containerPlacementLists.size() >= kListCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedLists;
        mark_identity_owner_incomplete(context);
        return catalog::kNoRow;
    }
    const std::uint32_t row =
        static_cast<std::uint32_t>(context.output->containerPlacementLists.size());
    context.output->containerPlacementLists.emplace_back();
    catalog::ContainerPlacementList& list = context.output->containerPlacementLists.back();
    zero_row_storage(list);
    list.objectListTag = tag;
    list.objectListNameRow = catalog::kNoRow;
    list.resourceNameRow = catalog::kNoRow;
    context.listRows.emplace(tag, row);
    materialize_list(context, row);
    return row;
}

/** Appends one exact container member edge with its independent authored mask. */
void append_owner(BuildContext& context,
                  std::uint32_t listRow,
                  std::uint32_t containerTag,
                  std::uint32_t memberIndex,
                  std::uint64_t scenarioMask,
                  const std::array<std::uint8_t, tables::kContainerBubbleMaskBytes>& mapMask) {
    if (context.output->containerPlacementOwners.size() >= kOwnerCapacity) {
        ++context.output->containerPlacementDiagnostics.droppedOwners;
        mark_identity_owner_incomplete(context);
        return;
    }
    context.output->containerPlacementOwners.emplace_back();
    catalog::ContainerPlacementOwner& owner = context.output->containerPlacementOwners.back();
    zero_row_storage(owner);
    owner.listRow = listRow;
    owner.containerTag = containerTag;
    owner.memberIndex = memberIndex;
    owner.containerNameRow = catalog::kNoRow;
    owner.scenarioBubbleMask = scenarioMask;
    owner.mapBubbleMask = mapMask;
    owner.context = catalog::SpatialContextJoin::packageStemBubble;
}

/** Reads one selected-stem container and follows every exact object-list member. */
[[nodiscard]] bool collect_container_impl(BuildContext& context, std::uint32_t containerTag) {
    if (cancelled(context)) {
        context.failed = true;
        return false;
    }
    if (!read_exact(context, containerTag, tables::kContainerClass, context.containerBytes)) {
        mark_identity_owner_incomplete(context);
        return !context.failed;
    }
    std::array<std::uint8_t, tables::kContainerBubbleMaskBytes> mapMask{};
    tables::Array members{};
    if (!tables::container_bubble_mask(context.containerBytes, mapMask)
        || !tables::container_members(context.containerBytes, members)
        || members.elementClass != tables::kContainerMemberClass) {
        mark_identity_owner_unresolved(context);
        return true;
    }
    const std::uint64_t scenarioMask = scenario_bubble_mask(*context.output, mapMask);
    if (scenarioMask == 0) {
        return true;
    }
    for (std::size_t index = 0; index < members.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        std::uint32_t memberTag = 0;
        std::uint32_t memberClass = 0;
        if (!tables::container_member_at(context.containerBytes, members, index, memberTag)) {
            mark_identity_owner_unresolved(context);
            break;
        }
        if (!read_tag(context, memberTag, context.listBytes, memberClass)) {
            mark_identity_owner_unresolved(context);
            continue;
        }
        if (memberClass != tables::kAuthoredPlacementListClass) {
            continue;
        }
        const std::uint32_t listRow = ensure_list(context, memberTag);
        if (listRow != catalog::kNoRow) {
            append_owner(context,
                         listRow,
                         containerTag,
                         static_cast<std::uint32_t>(index),
                         scenarioMask,
                         mapMask);
        }
    }
    return !context.failed;
}

/** Converts exceptions at the container boundary into one failed build. */
[[nodiscard]] bool collect_container(BuildContext& context, std::uint32_t containerTag) noexcept {
    try {
        return collect_container_impl(context, containerTag);
    } catch (...) {
        mark_identity_owner_incomplete(context);
        context.failed = true;
        return false;
    }
}

/** Implements one container-placement build with optional pass-local graph reuse. */
[[nodiscard]] bool append_container_placements_impl(const package_reader::Source& source,
                                                    package_reader::Scratch& scratch,
                                                    ContainerPlacementCache* cache,
                                                    const ContainerIndex& containers,
                                                    const scenario_layouts::LayoutSet* layouts,
                                                    std::string_view scenarioName,
                                                    catalog::Snapshot& output,
                                                    ContainerPlacementCancelCheck cancel) noexcept {
    output.containerPlacementDiagnostics = {};
    state::build_data::scenarios::Definition scenario{};
    if (!scenario_layouts::find_layout(layouts, scenarioName, scenario)) {
        output.containerPlacementDiagnostics.unresolvedReads = 1;
        return true;
    }
    if (scenario.spawnStemLength == 0) {
        output.containerPlacementDiagnostics.contextNotApplicable = true;
        output.containerPlacementDiagnostics.identityOwnerInventoryComplete = true;
        output.containerPlacementDiagnostics.complete = true;
        return true;
    }
    try {
        BuildContext context{};
        context.source = &source;
        context.scratch = &scratch;
        context.output = &output;
        context.cache = cache;
        context.cancel = cancel;
        context.stem = std::string_view(scenario.spawnStem.data(), scenario.spawnStemLength);
        auto& diagnostics = output.containerPlacementDiagnostics;
        diagnostics.contextResolved = true;
        diagnostics.identityOwnerInventoryComplete = true;
        diagnostics.complete = true;
        output.containerPlacementLists.reserve(512);
        output.containerPlacementOwners.reserve(1'024);
        output.containerPlacements.reserve(8'192);
        output.containerPlacementConfigs.reserve(32'768);
        output.containerPlacementComponents.reserve(32'768);
        context.listRows.reserve(1'024);
        // The index is swept once per pass. A refusal there is one unresolved read here, not one
        // per package, because no scenario can see what the sweep never reported.
        if (!containers.complete || !containers.stemsComplete) {
            mark_identity_owner_unresolved(context);
        }
        for (const ContainerIndexEntry& entry : containers.entries) {
            if (context.failed || cancelled(context)) {
                break;
            }
            if (same_stem(context, entry) && !collect_container(context, entry.tag)) {
                break;
            }
        }
        const bool finished = !context.failed && !cancelled(context);
        if (!finished) {
            mark_identity_owner_incomplete(context);
        }
        return finished;
    } catch (...) {
        output.containerPlacementDiagnostics.identityOwnerInventoryComplete = false;
        output.containerPlacementDiagnostics.complete = false;
        return false;
    }
}

} // namespace

/** Allocates optional pass storage without making extraction depend on it. */
ContainerPlacementCache::ContainerPlacementCache() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_.reset();
    }
}

ContainerPlacementCache::~ContainerPlacementCache() = default;

/** Appends the selected-stem container placement graph without assigning ClientRef identity. */
bool append_container_placements(const package_reader::Source& source,
                                 package_reader::Scratch& scratch,
                                 const ContainerIndex& containers,
                                 const scenario_layouts::LayoutSet* layouts,
                                 std::string_view scenarioName,
                                 catalog::Snapshot& output,
                                 ContainerPlacementCancelCheck cancel) noexcept {
    return append_container_placements_impl(
        source, scratch, nullptr, containers, layouts, scenarioName, output, cancel);
}

/** Appends container placements with exact pass-local object-list graph reuse. */
bool append_container_placements(const package_reader::Source& source,
                                 package_reader::Scratch& scratch,
                                 ContainerPlacementCache& cache,
                                 const ContainerIndex& containers,
                                 const scenario_layouts::LayoutSet* layouts,
                                 std::string_view scenarioName,
                                 catalog::Snapshot& output,
                                 ContainerPlacementCancelCheck cancel) noexcept {
    return append_container_placements_impl(
        source, scratch, &cache, containers, layouts, scenarioName, output, cancel);
}

} // namespace sunrise::client::content::activity::scriptables::internal
