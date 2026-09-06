#include "authored_placement_marker.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <memory>
#include <vector>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../core/ui/world_marker/projection.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../hooks/teleport/runtime.h"
#include "authored_placement_marker_draw.h"
#include "authored_placement_marker_settings_store.h"
#include "package_aabb_marker_source.h"
#include "package_embedded_placement_marker_source.h"
#include "package_trigger_volume_marker_source.h"
#include "package_type23_placement_marker_source.h"

namespace sunrise::client::ui::activity::authored_placement_marker {
namespace {

namespace projection = core::ui::world_marker;
namespace scaling = core::ui::scaling::dpi;
namespace package_aabb = package_aabb_marker_source;
namespace package_embedded = package_embedded_placement_marker_source;
namespace package_type23 = package_type23_placement_marker_source;
namespace package_trigger_volume = package_trigger_volume_marker_source;
namespace sdk = state::activity_sdk;
namespace sdk_format = state::activity_sdk::format;
namespace teleport = sunrise::client::hooks::teleport;

/** Five degrees avoids labelling objects that are only near the screen centre. */
constexpr double kMinimumGazeCosine = 0.9961946980917455;
constexpr float kGazeViewportFraction = 0.04F;
constexpr float kMinimumGazeRadius = 24.0F;
constexpr float kMaximumGazeRadius = 96.0F;
/** Projected labels within this CSS-pixel radius share one stack. */
constexpr float kLabelCoincidenceRadius = 16.0F;

SRWLOCK g_lock{SRWLOCK_INIT};
State g_state{};
RenderDiagnostics g_renderDiagnostics{};
int g_visibleFrame{-1};
WorldPage g_worldPage{WorldPage::none};

/** Immutable drawable-row set published by one browser page. */
struct PublishedRows final {
    Context context{};
    std::vector<Anchor> anchors{};
    PublishedSource source{PublishedSource::authoredAndContainer};
};

/** One deterministic screen-space stack shared by labels from every marker source. */
struct LabelLayout final {
    struct Cluster final {
        projection::ScreenPoint origin{};
        std::size_t nextRow{};
    };

    struct Entry final {
        projection::ScreenPoint point{};
        std::size_t cluster{};
    };

    std::array<Cluster, kLabelCapacity> clusters{};
    std::array<Entry, kLabelCapacity> entries{};
    std::size_t clusterCount{};
    std::size_t entryCount{};
};

std::shared_ptr<const PublishedRows> g_publishedRows{};

/** Places one label in the first nearby cluster without moving its marker glyph. */
[[nodiscard]] projection::ScreenPoint stacked_label_point(LabelLayout& layout,
                                                          const projection::ScreenPoint& point,
                                                          float rowSpacing) noexcept {
    const float radius = scaling::pixels(kLabelCoincidenceRadius);
    const double radiusSquared = static_cast<double>(radius) * static_cast<double>(radius);
    std::size_t cluster = layout.clusters.size();
    for (std::size_t index = 0; index < layout.entryCount; ++index) {
        const double x = static_cast<double>(point.x) - layout.entries[index].point.x;
        const double y = static_cast<double>(point.y) - layout.entries[index].point.y;
        if (x * x + y * y <= radiusSquared) {
            cluster = layout.entries[index].cluster;
            break;
        }
    }
    if (cluster == layout.clusters.size()) {
        if (layout.clusterCount == layout.clusters.size()) {
            return point;
        }
        cluster = layout.clusterCount++;
        layout.clusters[cluster] = {point, 0};
    }
    projection::ScreenPoint output = layout.clusters[cluster].origin;
    output.y += rowSpacing * static_cast<float>(layout.clusters[cluster].nextRow++);
    if (layout.entryCount < layout.entries.size()) {
        layout.entries[layout.entryCount++] = {point, cluster};
    }
    return output;
}

/** Publishes one bounded render-set result for the workbench status line. */
void publish_render_diagnostics(const RenderSet& source) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_renderDiagnostics.glyphs = source.count;
    g_renderDiagnostics.sourceRowsVisited = source.sourceRowsVisited;
    g_renderDiagnostics.glyphsCapped = source.capped;
    g_renderDiagnostics.sourceScanCapped = source.sourceScanCapped;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return True when one fixed-size digest contains any identity byte. */
[[nodiscard]] bool has_digest(const std::array<std::byte, 32>& digest) noexcept {
    return std::any_of(digest.begin(), digest.end(), [](std::byte value) noexcept {
        return value != std::byte{};
    });
}

/** @return True when two values name the same activity and source-catalog generation. */
[[nodiscard]] bool same_context(const Context& left, const Context& right) noexcept {
    if (left.activity.sessionId != right.activity.sessionId
        || left.activity.createdRevision != right.activity.createdRevision
        || left.scenarioTag != right.scenarioTag || left.catalogKind != right.catalogKind) {
        return false;
    }
    if (left.catalogKind == CatalogKind::dynamicScriptables) {
        return left.dynamicCatalogRevision == right.dynamicCatalogRevision;
    }
    return left.sdkActivityRow == right.sdkActivityRow
           && left.sdkScenarioRow == right.sdkScenarioRow
           && left.sdkActivityClientGeneration == right.sdkActivityClientGeneration
           && left.sdkLogicalIrSha256 == right.sdkLogicalIrSha256;
}

/** @return True when a selection context has every required exact identity. */
[[nodiscard]] bool valid_context(const Context& context) noexcept {
    if (context.activity.sessionId == state::activity::kAbsentSessionId
        || context.activity.createdRevision == state::activity::kInvalidRevision
        || context.scenarioTag == 0) {
        return false;
    }
    if (context.catalogKind == CatalogKind::dynamicScriptables) {
        return context.dynamicCatalogRevision != 0
               && context.sdkActivityRow == sdk_format::kAbsentIndex
               && context.sdkScenarioRow == sdk_format::kAbsentIndex
               && context.sdkActivityClientGeneration == 0
               && !has_digest(context.sdkLogicalIrSha256);
    }
    if (context.catalogKind != CatalogKind::activitySdk) {
        return false;
    }
    return context.dynamicCatalogRevision == 0 && context.sdkActivityRow != sdk_format::kAbsentIndex
           && context.sdkScenarioRow != sdk_format::kAbsentIndex
           && context.sdkActivityClientGeneration != 0 && has_digest(context.sdkLogicalIrSha256);
}

/** @return True when one source kind belongs to its explicitly named catalog family. */
[[nodiscard]] bool source_matches_context(const Context& context, AnchorSource source) noexcept {
    return (context.catalogKind == CatalogKind::activitySdk)
           == (source == AnchorSource::sdkSquadAnchor);
}

/** @return True when one marker kind belongs to the selected SDK tab. */
[[nodiscard]] bool page_accepts(WorldPage page, AnchorSource source) noexcept {
    switch (page) {
    case WorldPage::none:
    case WorldPage::objects:
        return true;
    case WorldPage::devices:
        return source == AnchorSource::packageType23Placement
               || source == AnchorSource::packageEmbeddedPlacement;
    case WorldPage::triggers:
        return source == AnchorSource::packageTriggerVolume;
    case WorldPage::positions:
        return source == AnchorSource::authoredPlacement
               || source == AnchorSource::containerPlacement;
    case WorldPage::squads:
        return source == AnchorSource::sdkSquadAnchor;
    }
    return false;
}

/** @return True when one source uses its parent row as part of exact identity. */
[[nodiscard]] bool owner_scoped_source(AnchorSource source) noexcept {
    return source == AnchorSource::packageAabb || source == AnchorSource::packageTriggerVolume
           || source == AnchorSource::packageType23Placement
           || source == AnchorSource::packageEmbeddedPlacement
           || source == AnchorSource::sdkSquadAnchor;
}

/** @return True when one position anchor may enter the renderer. */
[[nodiscard]] bool valid_anchor(const Anchor& anchor) noexcept {
    if (anchor.sourceKind == AnchorSource::sdkSquadAnchor) {
        return anchor.sourceRow != sdk_format::kAbsentIndex
               && anchor.ownerRow != sdk_format::kAbsentIndex
               && anchor.slotRow != sdk_format::kAbsentIndex && anchor.objectListTag != 0
               && anchor.objectListTag != sdk_format::kAbsentIndex
               && anchor.placementIdentifier != 0 && anchor.placementIdentifier != UINT64_MAX
               && std::all_of(anchor.position.begin(), anchor.position.end(), [](float value) {
                      return std::isfinite(value);
                  });
    }
    if (anchor.sourceKind == AnchorSource::packageTriggerVolume
        || anchor.sourceKind == AnchorSource::packageType23Placement
        || anchor.sourceKind == AnchorSource::packageEmbeddedPlacement) {
        return anchor.sourceRow != state::build_data::scriptables::kNoRow
               && anchor.ownerRow != state::build_data::scriptables::kNoRow
               && anchor.slotRow != state::build_data::scriptables::kNoRow && anchor.configTag != 0;
    }
    const bool sourceValid =
        anchor.sourceKind == AnchorSource::containerPlacement || anchor.stateEntryTag != 0;
    return anchor.sourceKind != AnchorSource::packageAabb && sourceValid
           && anchor.sourceRow != state::build_data::scriptables::kNoRow
           && anchor.objectListTag != 0;
}

/** @return True when two anchors name the same exact catalog row. */
[[nodiscard]] bool same_anchor_identity(const Anchor& left, const Anchor& right) noexcept {
    return left.sourceKind == right.sourceKind && left.sourceRow == right.sourceRow
           && (!owner_scoped_source(left.sourceKind) || left.ownerRow == right.ownerRow);
}

/** @return True when the exact catalog shared by a selection set remains current. */
[[nodiscard]] bool catalog_context_current(const state::build_data::scriptables::Snapshot& catalog,
                                           const Context& context) noexcept {
    return context.catalogKind == CatalogKind::dynamicScriptables
           && catalog.revision == context.dynamicCatalogRevision
           && catalog.scenarioTag == context.scenarioTag
           && catalog.status == state::build_data::scriptables::BuildStatus::ready;
}

/** @return True while one exact package row still matches its retained identity. */
[[nodiscard]] bool catalog_anchor_current(const state::build_data::scriptables::Snapshot& catalog,
                                          const Anchor& selection) noexcept {
    if (selection.sourceKind == AnchorSource::packageAabb) {
        return false;
    }
    if (selection.sourceKind == AnchorSource::packageTriggerVolume) {
        return package_trigger_volume::current(catalog, selection);
    }
    if (selection.sourceKind == AnchorSource::packageType23Placement) {
        return package_type23::current(catalog, selection);
    }
    if (selection.sourceKind == AnchorSource::packageEmbeddedPlacement) {
        return package_embedded::current(catalog, selection);
    }
    if (selection.sourceKind == AnchorSource::containerPlacement) {
        if (selection.sourceRow >= catalog.containerPlacements.size()) {
            return false;
        }
        const state::build_data::scriptables::ContainerPlacement& anchor =
            catalog.containerPlacements[selection.sourceRow];
        return anchor.objectListTag == selection.objectListTag
               && anchor.classListTag == selection.classListTag
               && anchor.entryIndex == selection.entryIndex;
    }
    if (selection.bubbleRow >= catalog.bubbles.size() || selection.stateRow >= catalog.states.size()
        || selection.sourceRow >= catalog.authoredPlacements.size()) {
        return false;
    }
    const state::build_data::scriptables::Bubble& bubble = catalog.bubbles[selection.bubbleRow];
    const state::build_data::scriptables::State& owner = catalog.states[selection.stateRow];
    const state::build_data::scriptables::AuthoredPlacement& anchor =
        catalog.authoredPlacements[selection.sourceRow];
    return bubble.index == selection.bubbleIndex && owner.bubbleRow == selection.bubbleRow
           && owner.entryTag == selection.stateEntryTag
           && owner.sliceSetIndex == selection.sliceSetIndex
           && anchor.bubbleRow == selection.bubbleRow && anchor.stateRow == selection.stateRow
           && anchor.objectListTag == selection.objectListTag
           && anchor.classListTag == selection.classListTag
           && anchor.entryIndex == selection.entryIndex;
}

/** Rebuilds one exact SDK-owned point from its global squad and anchor rows. */
[[nodiscard]] bool build_sdk_squad_anchor(const sdk::Catalog& catalog,
                                          std::uint32_t scenarioRow,
                                          std::uint32_t squadRow,
                                          std::uint32_t anchorRow,
                                          Anchor& output) noexcept {
    output = {};
    const auto scenarios = catalog.scenarios();
    const auto squads = catalog.squads();
    const auto anchors = catalog.squad_anchors();
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    const auto occurrences = catalog.occurrences();
    if (scenarioRow >= scenarios.size() || squadRow >= squads.size()
        || anchorRow >= anchors.size()) {
        return false;
    }
    const sdk_format::Squad& squad = squads[squadRow];
    if (squad.scenarioIndex != scenarioRow || squad.slotIndex >= slots.size()
        || squad.objectIndex >= objects.size() || squad.occurrenceIndex >= occurrences.size()
        || anchorRow < squad.anchors.first
        || anchorRow - squad.anchors.first >= squad.anchors.count) {
        return false;
    }
    const sdk_format::Slot& slot = slots[squad.slotIndex];
    const sdk_format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
    const sdk_format::SquadAnchor& anchor = anchors[anchorRow];
    const std::uint32_t childOrdinal = anchorRow - squad.anchors.first;
    if (slot.objectIndex != squad.objectIndex || occurrence.scenarioIndex != scenarioRow
        || occurrence.objectIndex != squad.objectIndex || anchor.squadIndex != squadRow
        || anchor.pointOrdinal != childOrdinal
        || (anchor.flags & sdk_format::kSquadAnchorExact) == 0) {
        return false;
    }

    output.sourceKind = AnchorSource::sdkSquadAnchor;
    output.sourceRow = anchorRow;
    output.ownerRow = squadRow;
    output.slotRow = squad.slotIndex;
    output.objectListTag = anchor.objectListTag;
    output.entryIndex = anchor.placementOrdinal;
    output.placementIdentifier = anchor.placedEntryIdentity;
    for (std::size_t lane = 0; lane < output.position.size(); ++lane) {
        output.position[lane] = std::bit_cast<float>(anchor.positionBits[lane]);
    }
    return valid_anchor(output);
}

/** @return True while one retained SDK anchor is still the exact catalog row it named. */
[[nodiscard]] bool sdk_anchor_current(const sdk::Catalog& catalog,
                                      const Context& context,
                                      const Anchor& selection) noexcept {
    if (selection.sourceKind != AnchorSource::sdkSquadAnchor) {
        return false;
    }
    Anchor current{};
    if (!build_sdk_squad_anchor(
            catalog, context.sdkScenarioRow, selection.ownerRow, selection.sourceRow, current)) {
        return false;
    }
    return same_anchor_identity(current, selection) && current.slotRow == selection.slotRow
           && current.objectListTag == selection.objectListTag
           && current.entryIndex == selection.entryIndex
           && current.placementIdentifier == selection.placementIdentifier
           && current.position == selection.position;
}

/** Resolves and revalidates the current SDK mapping and ActivityClient generation. */
[[nodiscard]] bool current_sdk_catalog(const Context& context, sdk::Snapshot& catalog) noexcept {
    catalog.reset();
    if (!valid_context(context) || context.catalogKind != CatalogKind::activitySdk
        || sdk::status() != sdk::Status::ready) {
        return false;
    }
    catalog = sdk::snapshot();
    if (catalog == nullptr) {
        return false;
    }
    const std::span<const std::byte> digest = catalog->logical_ir_sha256();
    if (digest.size() != context.sdkLogicalIrSha256.size()
        || !std::equal(digest.begin(), digest.end(), context.sdkLogicalIrSha256.begin())) {
        catalog.reset();
        return false;
    }
    sdk::BoundView view{};
    view.catalog = catalog;
    view.binding = context.activity;
    view.activityClientGeneration = context.sdkActivityClientGeneration;
    view.activityRow = context.sdkActivityRow;
    view.scenarioRow = context.sdkScenarioRow;
    const sdk_format::Scenario* const scenario = sdk::bound_scenario(view);
    const sdk_format::Activity* const activity = sdk::bound_activity(view);
    if (scenario == nullptr || activity == nullptr || scenario->tag != context.scenarioTag
        || activity->scenarioIndex != context.sdkScenarioRow) {
        catalog.reset();
        return false;
    }
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(context.activity, link);
    if (sdk::revalidate(view, context.activity, link.matchingLinks, link.activityClientGeneration)
        != sdk::Status::ready) {
        catalog.reset();
        return false;
    }
    return true;
}

/** @return True when a projected point is inside the bounded centre-screen gaze cone. */
[[nodiscard]] bool camera_looks_at(const Anchor& anchor,
                                   const teleport::CameraPose& source,
                                   const projection::ScreenPoint& point,
                                   const projection::Viewport& area) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(area.x)
        || !std::isfinite(area.y) || !std::isfinite(area.width) || !std::isfinite(area.height)
        || area.width <= 0.0F || area.height <= 0.0F) {
        return false;
    }
    const double centreX = static_cast<double>(area.x) + static_cast<double>(area.width) * 0.5;
    const double centreY = static_cast<double>(area.y) + static_cast<double>(area.height) * 0.5;
    const double screenX = static_cast<double>(point.x) - centreX;
    const double screenY = static_cast<double>(point.y) - centreY;
    const float gazeRadius = std::clamp((std::min)(area.width, area.height) * kGazeViewportFraction,
                                        scaling::pixels(kMinimumGazeRadius),
                                        scaling::pixels(kMaximumGazeRadius));
    const double gazeRadiusSquared = static_cast<double>(gazeRadius) * gazeRadius;
    if (screenX * screenX + screenY * screenY > gazeRadiusSquared) {
        return false;
    }

    double directionLengthSquared = 0.0;
    double forwardLengthSquared = 0.0;
    double alignment = 0.0;
    for (std::size_t lane = 0; lane < anchor.position.size(); ++lane) {
        if (!std::isfinite(anchor.position[lane]) || !std::isfinite(source.position[lane])
            || !std::isfinite(source.forward[lane])) {
            return false;
        }
        const double direction =
            static_cast<double>(anchor.position[lane]) - static_cast<double>(source.position[lane]);
        const double forward = static_cast<double>(source.forward[lane]);
        directionLengthSquared += direction * direction;
        forwardLengthSquared += forward * forward;
        alignment += direction * forward;
    }
    if (alignment <= 0.0 || directionLengthSquared <= 0.0 || forwardLengthSquared <= 0.0) {
        return false;
    }
    const double cosine = alignment / std::sqrt(directionLengthSquared * forwardLengthSquared);
    return std::isfinite(cosine) && cosine >= kMinimumGazeCosine;
}

/** Copies one current source row into the compact point-marker form. */
[[nodiscard]] bool catalog_anchor(const state::build_data::scriptables::Snapshot& catalog,
                                  AnchorSource sourceKind,
                                  std::uint32_t placementRow,
                                  std::uint32_t ownerRow,
                                  Anchor& output) noexcept {
    output = {};
    output.sourceKind = sourceKind;
    output.sourceRow = placementRow;
    output.ownerRow = ownerRow;
    if (sourceKind == AnchorSource::packageAabb) {
        return package_aabb::build(catalog, ownerRow, placementRow, output) && valid_anchor(output);
    }
    if (sourceKind == AnchorSource::packageTriggerVolume) {
        return package_trigger_volume::build(catalog, ownerRow, placementRow, output)
               && valid_anchor(output);
    }
    if (sourceKind == AnchorSource::packageType23Placement) {
        return package_type23::build(catalog, placementRow, output) && valid_anchor(output);
    }
    if (sourceKind == AnchorSource::packageEmbeddedPlacement) {
        return package_embedded::build(catalog, placementRow, output) && valid_anchor(output);
    }
    if (sourceKind == AnchorSource::containerPlacement) {
        if (placementRow >= catalog.containerPlacements.size()) {
            return false;
        }
        const state::build_data::scriptables::ContainerPlacement& placement =
            catalog.containerPlacements[placementRow];
        output.objectListTag = placement.objectListTag;
        output.classListTag = placement.classListTag;
        output.entryIndex = placement.entryIndex;
        output.position = placement.position;
        return valid_anchor(output);
    }
    if (placementRow >= catalog.authoredPlacements.size()) {
        return false;
    }
    const state::build_data::scriptables::AuthoredPlacement& placement =
        catalog.authoredPlacements[placementRow];
    if (placement.sourceObjectRow >= catalog.objects.size()
        || placement.stateRow >= catalog.states.size()
        || placement.bubbleRow >= catalog.bubbles.size()) {
        return false;
    }
    const state::build_data::scriptables::Object& object =
        catalog.objects[placement.sourceObjectRow];
    if (object.stateRow != placement.stateRow || object.bubbleRow != placement.bubbleRow) {
        return false;
    }
    const state::build_data::scriptables::State& owner = catalog.states[placement.stateRow];
    const state::build_data::scriptables::Bubble& bubble = catalog.bubbles[placement.bubbleRow];
    output.bubbleRow = placement.bubbleRow;
    output.bubbleIndex = bubble.index;
    output.stateRow = placement.stateRow;
    output.stateEntryTag = owner.entryTag;
    output.sliceSetIndex = owner.sliceSetIndex;
    output.objectListTag = placement.objectListTag;
    output.classListTag = placement.classListTag;
    output.entryIndex = placement.entryIndex;
    output.position = placement.position;
    return valid_anchor(output);
}

/** Draws one valid point when it passes radius and projection checks. */
[[nodiscard]] bool draw_anchor(const Anchor& anchor,
                               const state::build_data::scriptables::Snapshot& catalog,
                               const teleport::CameraPose& source,
                               const projection::Camera& camera,
                               const projection::Viewport& area,
                               const Options& options,
                               bool drawPointGlyph,
                               LabelLayout& labelLayout,
                               float labelRowSpacing,
                               std::size_t& labelled) noexcept {
    if (!catalog_anchor_current(catalog, anchor)) {
        return false;
    }
    projection::ScreenPoint point{};
    if (projection::project(anchor.position, camera, area, options.invertX, options.invertY, point)
        != projection::ProjectionStatus::visible) {
        return false;
    }
    const bool wantsLabel =
        options.alwaysShowLabels || camera_looks_at(anchor, source, point, area);
    const bool drawLabel = wantsLabel && labelled < kLabelCapacity;
    const projection::ScreenPoint labelPoint =
        drawLabel ? stacked_label_point(labelLayout, point, labelRowSpacing) : point;
    draw_detail::marker(point, labelPoint, anchor, catalog, options, drawPointGlyph, drawLabel);
    labelled += drawLabel ? 1U : 0U;
    return true;
}

/** Draws one direct SDK squad point after exact row revalidation and projection. */
[[nodiscard]] bool draw_sdk_anchor(const Anchor& anchor,
                                   const sdk::Catalog& catalog,
                                   const Context& context,
                                   const teleport::CameraPose& source,
                                   const projection::Camera& camera,
                                   const projection::Viewport& area,
                                   const Options& options,
                                   bool drawPointGlyph,
                                   LabelLayout& labelLayout,
                                   float labelRowSpacing,
                                   std::size_t& labelled) noexcept {
    if (!sdk_anchor_current(catalog, context, anchor)) {
        return false;
    }
    projection::ScreenPoint point{};
    if (projection::project(anchor.position, camera, area, options.invertX, options.invertY, point)
        != projection::ProjectionStatus::visible) {
        return false;
    }
    const bool wantsLabel =
        options.alwaysShowLabels || camera_looks_at(anchor, source, point, area);
    const bool drawLabel = wantsLabel && labelled < kLabelCapacity;
    const projection::ScreenPoint labelPoint =
        drawLabel ? stacked_label_point(labelLayout, point, labelRowSpacing) : point;
    draw_detail::sdk_squad_marker(
        point, labelPoint, anchor, catalog, options, drawPointGlyph, drawLabel);
    labelled += drawLabel ? 1U : 0U;
    return true;
}

} // namespace

/** Loads persistent presentation before any marker producer can publish session-bound rows. */
void initialize(void* module) noexcept {
    settings_store::initialize(module);
    State next{};
    next.options = settings_store::get();
    AcquireSRWLockExclusive(&g_lock);
    g_state = next;
    g_renderDiagnostics = {};
    g_visibleFrame = -1;
    g_worldPage = WorldPage::none;
    g_publishedRows.reset();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops every process- and session-bound marker row after the UI stops. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_state = {};
    g_renderDiagnostics = {};
    g_visibleFrame = -1;
    g_worldPage = WorldPage::none;
    g_publishedRows.reset();
    ReleaseSRWLockExclusive(&g_lock);
    settings_store::shutdown();
}

/** Selects one stable semantic colour without copying source identity into render rows. */
MarkerColor source_color(const Options& options, AnchorSource source) noexcept {
    switch (source) {
    case AnchorSource::authoredPlacement:
        return options.sourceColors.authoredPlacement;
    case AnchorSource::containerPlacement:
        return options.sourceColors.containerPlacement;
    case AnchorSource::packageTriggerVolume:
        return options.sourceColors.triggerVolume;
    case AnchorSource::packageType23Placement:
        return options.sourceColors.type23Placement;
    case AnchorSource::packageEmbeddedPlacement:
        return options.sourceColors.embeddedPlacement;
    case AnchorSource::sdkSquadAnchor:
        return options.sourceColors.sdkSquadAnchor;
    case AnchorSource::packageAabb:
        return {0.65F, 0.65F, 0.65F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

/** Compares the complete source-family-specific identity retained by two contexts. */
bool context_matches(const Context& left, const Context& right) noexcept {
    return same_context(left, right);
}

/** Captures one SDK mapping digest and the exact bound live identities it resolved against. */
bool sdk_context(const sdk::BoundView& view, Context& output) noexcept {
    output = {};
    const sdk_format::Activity* const activity = sdk::bound_activity(view);
    const sdk_format::Scenario* const scenario = sdk::bound_scenario(view);
    if (view.catalog == nullptr || activity == nullptr || scenario == nullptr
        || view.activityClientGeneration == 0 || scenario->tag == 0
        || activity->scenarioIndex != view.scenarioRow) {
        return false;
    }
    const std::span<const std::byte> digest = view.catalog->logical_ir_sha256();
    if (digest.size() != output.sdkLogicalIrSha256.size()) {
        return false;
    }
    output.activity = view.binding;
    output.dynamicCatalogRevision = 0;
    output.scenarioTag = scenario->tag;
    output.catalogKind = CatalogKind::activitySdk;
    output.sdkActivityRow = view.activityRow;
    output.sdkScenarioRow = view.scenarioRow;
    output.sdkActivityClientGeneration = view.activityClientGeneration;
    std::copy(digest.begin(), digest.end(), output.sdkLogicalIrSha256.begin());
    if (!valid_context(output)) {
        output = {};
        return false;
    }
    return true;
}

/** Builds one SDK point without translating it through the dynamic authored-placement catalog. */
bool sdk_squad_anchor(const sdk::BoundView& view,
                      std::uint32_t squadRow,
                      std::uint32_t anchorRow,
                      Anchor& output) noexcept {
    output = {};
    Context context{};
    if (!sdk_context(view, context)) {
        return false;
    }
    return build_sdk_squad_anchor(
        *view.catalog, context.sdkScenarioRow, squadRow, anchorRow, output);
}

/** Toggles one exact package row, replacing a selection set from a different context. */
void toggle(const Selection& selection) noexcept {
    if (!valid_context(selection.context) || !valid_anchor(selection.anchor)
        || !source_matches_context(selection.context, selection.anchor.sourceKind)) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (!same_context(g_state.context, selection.context)) {
        g_state.context = selection.context;
        g_state.selectionCount = 0;
        g_state.selectionCapped = false;
    }
    for (std::size_t index = 0; index < g_state.selectionCount; ++index) {
        if (!same_anchor_identity(g_state.anchors[index], selection.anchor)) {
            continue;
        }
        for (std::size_t cursor = index + 1; cursor < g_state.selectionCount; ++cursor) {
            g_state.anchors[cursor - 1] = g_state.anchors[cursor];
        }
        --g_state.selectionCount;
        g_state.selectionCapped = false;
        if (g_state.selectionCount == 0) {
            g_state.context = {};
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (g_state.selectionCount == g_state.anchors.size()) {
        g_state.selectionCapped = true;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_state.anchors[g_state.selectionCount++] = selection.anchor;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Replaces the bounded selection set in one exact context. */
std::size_t
select_many(const Context& context, std::span<const Anchor> anchors, bool sourceCapped) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_state.context = {};
    g_state.selectionCount = 0;
    g_state.selectionCapped = sourceCapped || anchors.size() > g_state.anchors.size();
    if (valid_context(context)) {
        g_state.context = context;
        for (const Anchor& anchor :
             anchors.first((std::min)(anchors.size(), g_state.anchors.size()))) {
            if (!valid_anchor(anchor) || !source_matches_context(context, anchor.sourceKind)) {
                continue;
            }
            if (anchor.sourceKind == AnchorSource::packageAabb) {
                continue;
            }
            const bool duplicate = std::any_of(
                g_state.anchors.begin(),
                g_state.anchors.begin() + static_cast<std::ptrdiff_t>(g_state.selectionCount),
                [&anchor](const Anchor& current) noexcept {
                    return same_anchor_identity(current, anchor);
                });
            if (!duplicate) {
                g_state.anchors[g_state.selectionCount++] = anchor;
            }
        }
    }
    if (g_state.selectionCount == 0) {
        g_state.context = {};
    }
    const std::size_t count = g_state.selectionCount;
    ReleaseSRWLockExclusive(&g_lock);
    return count;
}

/** @return True when a copied marker state contains one exact package row in this context. */
bool contains(const State& state,
              const Context& context,
              AnchorSource sourceKind,
              std::uint32_t sourceRow,
              std::uint32_t ownerRow) noexcept {
    if (!same_context(state.context, context)) {
        return false;
    }
    return std::any_of(state.anchors.begin(),
                       state.anchors.begin() + static_cast<std::ptrdiff_t>(state.selectionCount),
                       [sourceKind, sourceRow, ownerRow](const Anchor& anchor) noexcept {
                           return anchor.sourceKind == sourceKind && anchor.sourceRow == sourceRow
                                  && (!owner_scoped_source(sourceKind)
                                      || anchor.ownerRow == ownerRow);
                       });
}

/** @return True when one authored anchor is inside a finite radius from the given origin. */
bool in_radius(const Anchor& anchor, const std::array<float, 3>& origin, float radius) noexcept {
    if (!std::isfinite(radius) || radius <= 0.0F) {
        return false;
    }
    double distanceSquared = 0.0;
    for (std::size_t lane = 0; lane < anchor.position.size(); ++lane) {
        if (!std::isfinite(anchor.position[lane]) || !std::isfinite(origin[lane])) {
            return false;
        }
        const double delta =
            static_cast<double>(anchor.position[lane]) - static_cast<double>(origin[lane]);
        distanceSquared += delta * delta;
    }
    const double radiusSquared = static_cast<double>(radius) * static_cast<double>(radius);
    return distanceSquared <= radiusSquared;
}

/** Clears every selected anchor without changing presentation choices. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_state.context = {};
    g_state.anchors = {};
    g_state.selectionCount = 0;
    g_state.selectionCapped = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies bounded selection state for the workbench controls. */
State snapshot() noexcept {
    AcquireSRWLockShared(&g_lock);
    const State copy = g_state;
    ReleaseSRWLockShared(&g_lock);
    return copy;
}

/** Replaces and immediately saves marker presentation choices. */
void set_options(const Options& options) noexcept {
    preview_options(options);
    (void)settings_store::publish(options);
}

/** Applies an in-progress editor value without writing it to disk. */
void preview_options(const Options& options) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_state.options = options;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Records that a marker-owning page is visible in the current ImGui frame. */
void show_for_frame() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_visibleFrame = ImGui::GetFrameCount();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Records the selected SDK page. The published set belongs to a page, so a change drops it. */
void set_world_page(WorldPage page) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_worldPage != page) {
        g_worldPage = page;
        g_publishedRows.reset();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Saves only presentation; catalog identities and selected rows remain process-local. */
void save_options() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Options options = g_state.options;
    ReleaseSRWLockShared(&g_lock);
    (void)settings_store::publish(options);
}

/** Drops the published set. A page that cannot list its rows draws none. */
void publish_no_rows() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_publishedRows.reset();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Publishes one page's drawable rows without changing the persistent marker selection. */
bool publish_rows(const Context& context,
                  std::span<const Anchor> anchors,
                  PublishedSource source) noexcept {
    if (!valid_context(context)
        || (context.catalogKind == CatalogKind::activitySdk
            && source != PublishedSource::explicitRows)
        || std::any_of(anchors.begin(), anchors.end(), [&context](const Anchor& anchor) noexcept {
               return anchor.sourceKind == AnchorSource::packageAabb || !valid_anchor(anchor)
                      || !source_matches_context(context, anchor.sourceKind);
           })) {
        return false;
    }
    show_for_frame();
    AcquireSRWLockShared(&g_lock);
    const std::shared_ptr<const PublishedRows> current = g_publishedRows;
    ReleaseSRWLockShared(&g_lock);
    if (current != nullptr && same_context(current->context, context) && current->source == source
        && current->anchors.size() == anchors.size()
        && std::equal(current->anchors.begin(),
                      current->anchors.end(),
                      anchors.begin(),
                      [](const Anchor& left, const Anchor& right) noexcept {
                          return same_anchor_identity(left, right)
                                 && left.objectListTag == right.objectListTag
                                 && left.classListTag == right.classListTag
                                 && left.entryIndex == right.entryIndex
                                 && left.tableRow == right.tableRow
                                 && left.resourceTag == right.resourceTag
                                 && left.ownerMatchCount == right.ownerMatchCount
                                 && left.scenarioBubbleMask == right.scenarioBubbleMask
                                 && left.placementIdentifier == right.placementIdentifier
                                 && left.position == right.position;
                      })) {
        return true;
    }
    try {
        auto next = std::make_shared<PublishedRows>();
        next->context = context;
        next->source = source;
        next->anchors.assign(anchors.begin(), anchors.end());
        AcquireSRWLockExclusive(&g_lock);
        g_publishedRows = std::move(next);
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    } catch (...) {
        return false;
    }
}

/** Copies the exact bounded anchor set selected for this frame. */
bool render_set(RenderSet& output) noexcept {
    output = {};
    publish_render_diagnostics(output);
    const State current = snapshot();
    const int frame = ImGui::GetFrameCount();
    AcquireSRWLockShared(&g_lock);
    const WorldPage worldPage = g_worldPage;
    const bool visible = worldPage != WorldPage::none || g_visibleFrame == frame;
    const DisplayScope displayScope = current.options.displayScope;
    ReleaseSRWLockShared(&g_lock);
    if (!visible || !current.options.enabled
        || (displayScope == DisplayScope::selectedRows && current.selectionCount == 0)) {
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    const std::shared_ptr<const PublishedRows> publishedRows = g_publishedRows;
    ReleaseSRWLockShared(&g_lock);
    const bool selectedScope = displayScope == DisplayScope::selectedRows;
    if (!selectedScope && publishedRows == nullptr) {
        return false;
    }
    const Context& context = selectedScope ? current.context : publishedRows->context;
    state::build_data::scriptables::SnapshotView dynamicCatalog{};
    sdk::Snapshot sdkCatalog{};
    bool contextCurrent = false;
    if (context.catalogKind == CatalogKind::dynamicScriptables) {
        dynamicCatalog = state::build_data::scriptables::snapshot();
        contextCurrent = dynamicCatalog != nullptr
                         && catalog_context_current(*dynamicCatalog, context)
                         && state::activity::binding_matches(context.activity);
    } else {
        contextCurrent = current_sdk_catalog(context, sdkCatalog);
    }
    if (!contextCurrent) {
        return false;
    }
    output.context = context;
    output.options = current.options;
    output.options.displayScope = displayScope;

    teleport::CameraPose source{};
    const bool radiusScope = displayScope == DisplayScope::nearbyRows;
    if (radiusScope && !teleport::camera_pose(source)) {
        return false;
    }
    const auto append = [&](const Anchor& anchor) noexcept {
        const bool rowCurrent =
            context.catalogKind == CatalogKind::activitySdk
                ? sdkCatalog != nullptr && sdk_anchor_current(*sdkCatalog, context, anchor)
                : dynamicCatalog != nullptr && catalog_anchor_current(*dynamicCatalog, anchor);
        if (anchor.sourceKind == AnchorSource::packageAabb
            || !page_accepts(worldPage, anchor.sourceKind)
            || !source_matches_context(context, anchor.sourceKind) || !rowCurrent
            || (radiusScope && !in_radius(anchor, source.position, current.options.nearbyRadius))) {
            return true;
        }
        if (output.count == output.anchors.size()) {
            output.capped = true;
            return false;
        }
        output.anchors[output.count++] = anchor;
        return true;
    };
    if (selectedScope) {
        const std::size_t sourceLimit =
            (std::min)(current.selectionCount, kRenderSourceVisitCapacity);
        for (std::size_t index = 0; index < sourceLimit; ++index) {
            ++output.sourceRowsVisited;
            if (!append(current.anchors[index])) {
                break;
            }
        }
        output.sourceScanCapped =
            current.selectionCount > sourceLimit && output.sourceRowsVisited == sourceLimit;
    } else if (publishedRows->source == PublishedSource::explicitRows) {
        const std::size_t sourceLimit =
            (std::min)(publishedRows->anchors.size(), kRenderSourceVisitCapacity);
        for (std::size_t index = 0; index < sourceLimit; ++index) {
            ++output.sourceRowsVisited;
            if (!append(publishedRows->anchors[index])) {
                break;
            }
        }
        output.sourceScanCapped =
            publishedRows->anchors.size() > sourceLimit && output.sourceRowsVisited == sourceLimit;
    } else if (context.catalogKind == CatalogKind::dynamicScriptables) {
        const bool includeAuthored = publishedRows->source != PublishedSource::containerOnly;
        const bool includeContainer = publishedRows->source != PublishedSource::authoredOnly;
        const std::size_t authoredCount =
            includeAuthored ? dynamicCatalog->authoredPlacements.size() : 0;
        const std::size_t containerCount =
            includeContainer ? dynamicCatalog->containerPlacements.size() : 0;
        const std::size_t totalCount = authoredCount + containerCount;
        const std::size_t sourceLimit = (std::min)(totalCount, kRenderSourceVisitCapacity);
        for (std::size_t sourceRow = 0; sourceRow < sourceLimit; ++sourceRow) {
            ++output.sourceRowsVisited;
            const bool authored = sourceRow < authoredCount;
            const std::size_t placementRow = authored ? sourceRow : sourceRow - authoredCount;
            Anchor anchor{};
            const AnchorSource sourceKind =
                authored ? AnchorSource::authoredPlacement : AnchorSource::containerPlacement;
            if (catalog_anchor(*dynamicCatalog,
                               sourceKind,
                               static_cast<std::uint32_t>(placementRow),
                               state::build_data::scriptables::kNoRow,
                               anchor)
                && !append(anchor)) {
                break;
            }
        }
        output.sourceScanCapped =
            totalCount > sourceLimit && output.sourceRowsVisited == sourceLimit;
    } else {
        return false;
    }
    publish_render_diagnostics(output);
    return output.count != 0;
}

/** Copies the limits observed by the most recent render-set request. */
RenderDiagnostics render_diagnostics() noexcept {
    AcquireSRWLockShared(&g_lock);
    const RenderDiagnostics copy = g_renderDiagnostics;
    ReleaseSRWLockShared(&g_lock);
    return copy;
}

/** Draws labels and an optional depthless 2D fallback. */
bool draw(const RenderSet& current,
          const teleport::CameraPose& source,
          bool drawPointGlyphs) noexcept {
    state::build_data::scriptables::SnapshotView dynamicCatalog{};
    sdk::Snapshot sdkCatalog{};
    if (current.context.catalogKind == CatalogKind::dynamicScriptables) {
        dynamicCatalog = state::build_data::scriptables::snapshot();
        if (dynamicCatalog == nullptr
            || !catalog_context_current(*dynamicCatalog, current.context)) {
            return false;
        }
    } else if (!current_sdk_catalog(current.context, sdkCatalog)) {
        return false;
    }
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }
    const projection::Camera camera{
        source.position, source.forward, source.up, source.horizontalFov, source.aspect};
    const projection::Viewport area{
        viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y};
    std::size_t drawn = 0;
    std::size_t labelled = 0;
    LabelLayout labelLayout{};
    const float labelRowSpacing = ImGui::GetTextLineHeightWithSpacing();
    for (std::size_t index = 0; index < current.count; ++index) {
        if (current.context.catalogKind == CatalogKind::activitySdk) {
            drawn += draw_sdk_anchor(current.anchors[index],
                                     *sdkCatalog,
                                     current.context,
                                     source,
                                     camera,
                                     area,
                                     current.options,
                                     drawPointGlyphs,
                                     labelLayout,
                                     labelRowSpacing,
                                     labelled)
                         ? 1U
                         : 0U;
        } else {
            drawn += draw_anchor(current.anchors[index],
                                 *dynamicCatalog,
                                 source,
                                 camera,
                                 area,
                                 current.options,
                                 drawPointGlyphs,
                                 labelLayout,
                                 labelRowSpacing,
                                 labelled)
                         ? 1U
                         : 0U;
        }
    }
    return drawn != 0;
}

} // namespace sunrise::client::ui::activity::authored_placement_marker
