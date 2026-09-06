#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/activity/definition.h"

namespace sunrise::client::hooks::teleport {
struct CameraPose;
}

namespace sunrise::state::activity_sdk {
struct BoundView;
class Catalog;
} // namespace sunrise::state::activity_sdk

namespace sunrise::client::ui::activity::authored_placement_marker {

/** Hard bound for package anchors retained by the read-only marker overlay. */
inline constexpr std::size_t kSelectionCapacity = 512;
/** Hard bound for package-anchor glyphs submitted in one rendered frame. */
inline constexpr std::size_t kRenderCapacity = 256;
/** Hard bound for package rows inspected by one render-set request. */
inline constexpr std::size_t kRenderSourceVisitCapacity = 16'384;
/** Every bounded glyph may carry a label when the user disables label filtering. */
inline constexpr std::size_t kLabelCapacity = kRenderCapacity;

/** Catalog family that owns every row in one marker selection context. */
enum class CatalogKind : std::uint8_t {
    dynamicScriptables,
    activitySdk,
};

/** Exact activity and source-catalog generation shared by one anchor selection set. */
struct Context final {
    state::activity::SessionBinding activity{};
    std::uint64_t dynamicCatalogRevision{};
    std::uint32_t scenarioTag{};
    CatalogKind catalogKind{CatalogKind::dynamicScriptables};
    std::uint32_t sdkActivityRow{0xFFFFFFFFU};
    std::uint32_t sdkScenarioRow{0xFFFFFFFFU};
    std::uint64_t sdkActivityClientGeneration{};
    std::array<std::byte, 32> sdkLogicalIrSha256{};
};

/** Package catalog surface that owns one exact anchor row. */
enum class AnchorSource : std::uint8_t {
    authoredPlacement,
    containerPlacement,
    packageAabb,
    packageTriggerVolume,
    packageType23Placement,
    packageEmbeddedPlacement,
    sdkSquadAnchor,
};

/** Package source universe used only by the bounded nearby-row scan. */
enum class PublishedSource : std::uint8_t {
    authoredAndContainer,
    authoredOnly,
    containerOnly,
    explicitRows,
};

/** SDK tab that owns the current world overlay. */
enum class WorldPage : std::uint8_t {
    none,
    objects,
    devices,
    triggers,
    positions,
    squads,
};

/** One package-authored anchor selected independently of live objects and ClientRef rows. */
struct Anchor final {
    AnchorSource sourceKind{AnchorSource::authoredPlacement};
    std::uint32_t sourceRow{0xFFFFFFFFU};
    std::uint32_t ownerRow{0xFFFFFFFFU};
    std::uint32_t slotRow{0xFFFFFFFFU};
    std::uint32_t bubbleRow{0xFFFFFFFFU};
    std::uint32_t bubbleIndex{};
    std::uint32_t stateRow{0xFFFFFFFFU};
    std::uint32_t stateEntryTag{};
    std::uint32_t sliceSetIndex{};
    std::uint32_t objectListTag{};
    std::uint32_t configTag{};
    std::uint32_t classListTag{};
    std::uint32_t entryIndex{};
    std::uint32_t tableRow{0xFFFFFFFFU};
    std::uint32_t resourceTag{};
    std::uint32_t ownerMatchCount{};
    std::uint64_t scenarioBubbleMask{};
    std::uint64_t placementIdentifier{};
    std::array<float, 3> position{};
    std::array<float, 3> boundsMinimum{};
    std::array<float, 3> boundsMaximum{};
};

/** Exact context and one anchor used by the row-toggle API. */
struct Selection final {
    Context context{};
    Anchor anchor{};
};

/**
 * What is drawn in the world. It is independent of what a page lists and of which row is
 * highlighted. A page's published row set never depends on its search.
 */
enum class DisplayScope : std::uint8_t {
    selectedRows,
    publishedRows,
    nearbyRows,
};

/** World-line glyphs available when an anchor has only a proved position. */
enum class WorldGlyph : std::uint8_t {
    point,
    axes,
    diagnosticBox,
    diagnosticSphere,
};

/** One editable RGBA colour, with channels in the closed zero-to-one range. */
using MarkerColor = std::array<float, 4>;

/** Stable semantic colours for each renderable package-position source. */
struct SourceColors final {
    MarkerColor authoredPlacement{0.9569F, 0.7255F, 0.2588F, 1.0F};
    MarkerColor containerPlacement{0.7020F, 0.5725F, 0.9412F, 1.0F};
    MarkerColor triggerVolume{1.0F, 0.3608F, 0.7843F, 1.0F};
    MarkerColor type23Placement{0.4941F, 0.9059F, 0.5294F, 1.0F};
    MarkerColor embeddedPlacement{0.3373F, 0.7059F, 0.9137F, 1.0F};
    MarkerColor sdkSquadAnchor{0.3020F, 0.8510F, 1.0F, 1.0F};
};

/** Bounds shared by the editor and the persistent settings validator. */
inline constexpr float kMinimumNearbyRadius = 1.0F;
inline constexpr float kMaximumNearbyRadius = 100'000.0F;
inline constexpr float kMinimumWorldGlyphSize = 0.05F;
inline constexpr float kMaximumWorldGlyphSize = 100'000.0F;
inline constexpr float kMinimumWorldLineWidth = 1.0F;
inline constexpr float kMaximumWorldLineWidth = 16.0F;

/** Presentation choices for the read-only foreground point marker. */
struct Options final {
    DisplayScope displayScope{DisplayScope::selectedRows};
    float nearbyRadius{100.0F};
    float worldGlyphSize{1.0F};
    float worldLineWidth{3.0F};
    SourceColors sourceColors{};
    WorldGlyph worldGlyph{WorldGlyph::axes};
    bool enabled{true};
    bool alwaysShowLabels{};
    bool invertX{};
    bool invertY{};
    bool onlyRenderableObjects{};
};

/** Current bounded selection set and presentation choices. */
struct State final {
    Context context{};
    std::array<Anchor, kSelectionCapacity> anchors{};
    std::size_t selectionCount{};
    Options options{};
    bool selectionCapped{};
};

/** Bounded anchors after the selected, filtered, or nearby display scope is applied. */
struct RenderSet final {
    Context context{};
    std::array<Anchor, kRenderCapacity> anchors{};
    std::size_t count{};
    std::size_t sourceRowsVisited{};
    Options options{};
    bool capped{};
    bool sourceScanCapped{};
};

/** Most recent render-set limits, retained for the workbench status line. */
struct RenderDiagnostics final {
    std::size_t glyphs{};
    std::size_t sourceRowsVisited{};
    bool glyphsCapped{};
    bool sourceScanCapped{};
};

/** Loads persistent marker presentation before the first UI frame. */
void initialize(void* module) noexcept;

/** Drops every process- and session-bound marker row after the UI stops. */
void shutdown() noexcept;

/** @return The semantic colour assigned to one exact package-position source. */
[[nodiscard]] MarkerColor source_color(const Options& options, AnchorSource source) noexcept;

/** @return True when both values name one exact activity and source-catalog generation. */
[[nodiscard]] bool context_matches(const Context& left, const Context& right) noexcept;

/** Captures one exact SDK catalog, scenario, binding, and ActivityClient generation. */
[[nodiscard]] bool sdk_context(const state::activity_sdk::BoundView& view,
                               Context& output) noexcept;

/** Builds one direct, point-only SDK squad anchor from exact global parent and child rows. */
[[nodiscard]] bool sdk_squad_anchor(const state::activity_sdk::BoundView& view,
                                    std::uint32_t squadRow,
                                    std::uint32_t anchorRow,
                                    Anchor& output) noexcept;

/** Toggles one exact package row, replacing a selection set from a different context. */
void toggle(const Selection& selection) noexcept;

/**
 * Replaces the bounded selection set in one exact context.
 * @param context Exact activity and package-catalog generation.
 * @param anchors Caller-owned package anchors to retain.
 * @param sourceCapped True when the caller had more matching rows than it could pass.
 * @return Number of unique valid anchors retained.
 */
[[nodiscard]] std::size_t select_many(const Context& context,
                                      std::span<const Anchor> anchors,
                                      bool sourceCapped = false) noexcept;

/** @return True when a copied marker state contains one exact source row in this context. */
[[nodiscard]] bool contains(const State& state,
                            const Context& context,
                            AnchorSource sourceKind,
                            std::uint32_t sourceRow,
                            std::uint32_t ownerRow = 0xFFFFFFFFU) noexcept;

/** @return True when one authored anchor is inside a finite radius from the given origin. */
[[nodiscard]] bool
in_radius(const Anchor& anchor, const std::array<float, 3>& origin, float radius) noexcept;

/** Clears every selected anchor without changing presentation choices. */
void clear() noexcept;

/** Copies bounded selection state for the workbench controls. */
[[nodiscard]] State snapshot() noexcept;

/** Replaces and immediately saves marker presentation choices. */
void set_options(const Options& options) noexcept;

/** Applies an in-progress editor value without writing it to disk. */
void preview_options(const Options& options) noexcept;

/** Allows selected markers to render only while their owning page is drawn this frame. */
void show_for_frame() noexcept;

/** Retains only the selected SDK tab's marker kinds while its window remains open. */
void set_world_page(WorldPage page) noexcept;

/** Saves the current presentation choices without retaining any selected package rows. */
void save_options() noexcept;

/**
 * Publishes the rows one page can draw. The set must not depend on that page's search or
 * filters, because those choose what the list shows, not what the world draws.
 */
[[nodiscard]] bool
publish_rows(const Context& context,
             std::span<const Anchor> anchors,
             PublishedSource source = PublishedSource::authoredAndContainer) noexcept;

/** Drops the published set. A page that cannot list its rows draws none. */
void publish_no_rows() noexcept;

/** Copies the exact bounded anchor set selected for this frame. */
[[nodiscard]] bool render_set(RenderSet& output) noexcept;

/** Copies the limits observed by the most recent render-set request. */
[[nodiscard]] RenderDiagnostics render_diagnostics() noexcept;

/** Draws labels and an optional 2D fallback for one already-resolved frame source. */
[[nodiscard]] bool draw(const RenderSet& current,
                        const hooks::teleport::CameraPose& source,
                        bool drawPointGlyphs = true) noexcept;

} // namespace sunrise::client::ui::activity::authored_placement_marker
