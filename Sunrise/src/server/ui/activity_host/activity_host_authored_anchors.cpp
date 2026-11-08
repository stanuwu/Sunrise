#include "activity_host_authored_anchors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/hooks/teleport/runtime.h"
#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../activity/host_runtime.h"
#include "activity_host_anchor_render_controls.h"
#include "activity_host_package_tag_names.h"
#include "activity_host_scriptable_labels.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::authored_anchors {
namespace {

namespace catalog = state::build_data::scriptables;
namespace render_controls = server::ui::activity_host::anchor_render_controls;
namespace marker = client::ui::activity::authored_placement_marker;
namespace tag_names = server::ui::activity_host::package_tag_names;
namespace labels = server::ui::activity_host::scriptable_labels;
namespace tables = middleware::content::packages::tables;
namespace teleport = client::hooks::teleport;

/** One shared table style, so every table on this page reads the same. */
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
/** Package-row sources offered by the bounded multi-selection controls. */
enum class RowSet : std::uint8_t {
    visible,
    all,
    nearby,
};

/** One browser row keeps its package surface distinct from every other source. */
struct BrowserRow final {
    marker::AnchorSource sourceKind{marker::AnchorSource::authoredPlacement};
    std::uint32_t sourceRow{catalog::kNoRow};
    std::uint32_t ownerRow{catalog::kNoRow};
};

std::vector<BrowserRow> g_visible{};
std::uint64_t g_materializedRevision{};
std::array<char, 256> g_materializedFilter{};
int g_materializedBubble{-2};
int g_materializedState{-2};
int g_materializedScope{-1};
int g_materializedSource{-1};
int g_sourceFilter{};

[[nodiscard]] const catalog::Name* name_row(const catalog::Snapshot& snapshot,
                                            std::uint32_t row) noexcept {
    return row < snapshot.names.size() ? &snapshot.names[row] : nullptr;
}

[[nodiscard]] std::string_view selected_name(const catalog::Snapshot& snapshot,
                                             std::uint32_t row) noexcept {
    const catalog::Name* name = name_row(snapshot, row);
    if (name == nullptr || name->selectedCandidate >= snapshot.nameCandidates.size()) {
        return {};
    }
    const catalog::NameCandidate& candidate = snapshot.nameCandidates[name->selectedCandidate];
    return {candidate.value.data(), candidate.length};
}

/** Draws a package name when one strongest candidate was unique. */
void draw_name(const catalog::Snapshot& snapshot, std::uint32_t row) noexcept {
    const std::string_view value = selected_name(snapshot, row);
    if (!value.empty()) {
        ImGui::Text("%.*s", static_cast<int>(value.size()), value.data());
    } else {
        ImGui::TextDisabled("Unnamed");
    }
}

/** @return True when any retained name candidate passes the browser text filter. */
[[nodiscard]] bool filter_name(const catalog::Snapshot& snapshot,
                               std::uint32_t nameRow,
                               const ImGuiTextFilter& filter) noexcept {
    const catalog::Name* name = name_row(snapshot, nameRow);
    if (name == nullptr) {
        return false;
    }
    const std::size_t end = static_cast<std::size_t>(name->firstCandidate) + name->candidateCount;
    for (std::size_t index = name->firstCandidate;
         index < end && index < snapshot.nameCandidates.size();
         ++index) {
        const catalog::NameCandidate& candidate = snapshot.nameCandidates[index];
        if (filter.PassFilter(candidate.value.data(), candidate.value.data() + candidate.length)) {
            return true;
        }
    }
    return false;
}

/** @return True when an authored placement's package provenance rows are all present. */
[[nodiscard]] bool placement_valid(const catalog::Snapshot& snapshot,
                                   const catalog::AuthoredPlacement& placement) noexcept {
    return placement.sourceObjectRow < snapshot.objects.size()
           && placement.stateRow < snapshot.states.size()
           && placement.bubbleRow < snapshot.bubbles.size()
           && snapshot.objects[placement.sourceObjectRow].stateRow == placement.stateRow
           && snapshot.objects[placement.sourceObjectRow].bubbleRow == placement.bubbleRow;
}

/** @return True when a package anchor passes the shared browser filters. */
[[nodiscard]] bool placement_matches(const catalog::Snapshot& snapshot,
                                     const catalog::AuthoredPlacement& placement,
                                     const Filters& filters) noexcept {
    if (!placement_valid(snapshot, placement)) {
        return false;
    }
    const catalog::Object& object = snapshot.objects[placement.sourceObjectRow];
    const catalog::State& owner = snapshot.states[placement.stateRow];
    const catalog::Bubble& bubble = snapshot.bubbles[placement.bubbleRow];
    if ((filters.bubbleIndex >= 0
         && bubble.index != static_cast<std::uint32_t>(filters.bubbleIndex))
        || (filters.stateIndex >= 0
            && owner.index != static_cast<std::uint32_t>(filters.stateIndex))
        || !labels::scope_matches(object.registryDescriptor, filters.scope)) {
        return false;
    }
    if (filters.text == nullptr || !filters.text->IsActive()) {
        return true;
    }
    if (filter_name(snapshot, owner.nameRow, *filters.text)
        || filter_name(snapshot, bubble.nameRow, *filters.text)
        || tag_names::matches(snapshot, object.objectNameRow, *filters.text)
        || tag_names::matches(snapshot, placement.objectListNameRow, *filters.text)
        || tag_names::matches(snapshot, placement.classListNameRow, *filters.text)) {
        return true;
    }
    std::array<char, 320> identity{};
    (void)std::snprintf(identity.data(),
                        identity.size(),
                        "authored anchor list 0x%08X entry %u class 0x%08X object 0x%08X "
                        "key 0x%08X %.3f %.3f %.3f",
                        static_cast<unsigned>(placement.objectListTag),
                        static_cast<unsigned>(placement.entryIndex),
                        static_cast<unsigned>(placement.classListTag),
                        static_cast<unsigned>(object.objectTag),
                        static_cast<unsigned>(object.registryKey),
                        static_cast<double>(placement.position[0]),
                        static_cast<double>(placement.position[1]),
                        static_cast<double>(placement.position[2]));
    return filters.text->PassFilter(identity.data());
}

/** @return The first exact owner row that satisfies the optional scenario-bubble filter. */
[[nodiscard]] std::uint32_t container_owner_row(const catalog::Snapshot& snapshot,
                                                const catalog::ContainerPlacement& placement,
                                                int bubbleIndex) noexcept {
    std::uint32_t bubbleRow = catalog::kNoRow;
    if (bubbleIndex >= 0) {
        for (std::size_t row = 0; row < snapshot.bubbles.size(); ++row) {
            if (snapshot.bubbles[row].index == static_cast<std::uint32_t>(bubbleIndex)) {
                bubbleRow = static_cast<std::uint32_t>(row);
                break;
            }
        }
        if (bubbleRow == catalog::kNoRow) {
            return catalog::kNoRow;
        }
    }
    for (std::size_t row = 0; row < snapshot.containerPlacementOwners.size(); ++row) {
        const catalog::ContainerPlacementOwner& owner = snapshot.containerPlacementOwners[row];
        if (owner.listRow == placement.listRow
            && (bubbleRow == catalog::kNoRow
                || catalog::container_placement_owner_applies(owner, bubbleRow))) {
            return static_cast<std::uint32_t>(row);
        }
    }
    return catalog::kNoRow;
}

/** @return True when one numeric package identity passes the text filter. */
[[nodiscard]] bool
filter_identity(const ImGuiTextFilter& filter, const char* kind, std::uint32_t value) noexcept {
    std::array<char, 48> text{};
    (void)std::snprintf(text.data(), text.size(), "%s 0x%08X", kind, value);
    return filter.PassFilter(text.data());
}

/** @return True when one container placement passes only its proved package fields. */
[[nodiscard]] bool container_matches(const catalog::Snapshot& snapshot,
                                     const catalog::ContainerPlacement& placement,
                                     const Filters& filters,
                                     std::uint32_t& ownerRow) noexcept {
    ownerRow = catalog::kNoRow;
    if (placement.listRow >= snapshot.containerPlacementLists.size() || filters.stateIndex >= 0
        || filters.scope > 0) {
        return false;
    }
    ownerRow = container_owner_row(snapshot, placement, filters.bubbleIndex);
    if (ownerRow == catalog::kNoRow) {
        return false;
    }
    if (filters.text == nullptr || !filters.text->IsActive()) {
        return true;
    }
    const ImGuiTextFilter& filter = *filters.text;
    const catalog::ContainerPlacementList& list =
        snapshot.containerPlacementLists[placement.listRow];
    const catalog::ContainerPlacementOwner& owner = snapshot.containerPlacementOwners[ownerRow];
    if (tag_names::matches(snapshot, list.objectListNameRow, filter)
        || tag_names::matches(snapshot, list.resourceNameRow, filter)
        || tag_names::matches(snapshot, owner.containerNameRow, filter)
        || tag_names::matches(snapshot, placement.classListNameRow, filter)) {
        return true;
    }
    const std::size_t configEnd =
        static_cast<std::size_t>(placement.firstConfig) + placement.configCount;
    if (configEnd <= snapshot.containerPlacementConfigs.size()) {
        for (std::size_t row = placement.firstConfig; row < configEnd; ++row) {
            const catalog::ContainerPlacementConfig& config =
                snapshot.containerPlacementConfigs[row];
            if (tag_names::matches(snapshot, config.configNameRow, filter)
                || filter_identity(filter, "config", config.configTag)) {
                return true;
            }
        }
    }
    std::array<char, 256> identity{};
    (void)std::snprintf(identity.data(),
                        identity.size(),
                        "container placement list 0x%08X entry %u class 0x%08X type %u "
                        "resource 0x%08X %.3f %.3f %.3f unlinked ClientRef",
                        placement.objectListTag,
                        placement.entryIndex,
                        placement.classListTag,
                        placement.objectType,
                        list.resourceTag,
                        static_cast<double>(placement.position[0]),
                        static_cast<double>(placement.position[1]),
                        static_cast<double>(placement.position[2]));
    return filter.PassFilter(identity.data());
}

/** Rebuilds filtered authored row indices as one commit. */
[[nodiscard]] bool materialize(const catalog::Snapshot& snapshot, const Filters& filters) noexcept {
    const char* const text = filters.text != nullptr ? filters.text->InputBuf : "";
    const bool current =
        g_materializedRevision == snapshot.revision && g_materializedBubble == filters.bubbleIndex
        && g_materializedState == filters.stateIndex && g_materializedScope == filters.scope
        && g_materializedSource == g_sourceFilter
        && std::strncmp(g_materializedFilter.data(), text, g_materializedFilter.size()) == 0;
    if (current) {
        return true;
    }
    try {
        std::vector<BrowserRow> visible{};
        visible.reserve(snapshot.authoredPlacements.size() + snapshot.containerPlacements.size());
        if (g_sourceFilter <= 1) {
            for (std::size_t index = 0; index < snapshot.authoredPlacements.size(); ++index) {
                if (placement_matches(snapshot, snapshot.authoredPlacements[index], filters)) {
                    visible.push_back({marker::AnchorSource::authoredPlacement,
                                       static_cast<std::uint32_t>(index),
                                       catalog::kNoRow});
                }
            }
        }
        if (g_sourceFilter == 0 || g_sourceFilter == 2) {
            for (std::size_t index = 0; index < snapshot.containerPlacements.size(); ++index) {
                std::uint32_t ownerRow = catalog::kNoRow;
                if (container_matches(
                        snapshot, snapshot.containerPlacements[index], filters, ownerRow)) {
                    visible.push_back({marker::AnchorSource::containerPlacement,
                                       static_cast<std::uint32_t>(index),
                                       ownerRow});
                }
            }
        }
        g_visible.swap(visible);
        g_materializedRevision = snapshot.revision;
        g_materializedBubble = filters.bubbleIndex;
        g_materializedState = filters.stateIndex;
        g_materializedScope = filters.scope;
        g_materializedSource = g_sourceFilter;
        g_materializedFilter = {};
        (void)std::snprintf(g_materializedFilter.data(), g_materializedFilter.size(), "%s", text);
        return true;
    } catch (...) {
        return false;
    }
}

/** Builds the exact activity and catalog identity shared by this browser view. */
[[nodiscard]] marker::Context
marker_context(const catalog::Snapshot& snapshot,
               const server::activity::host::InstanceSnapshot& instance) noexcept {
    return {instance.binding, snapshot.revision, snapshot.scenarioTag};
}

/** Copies one valid package row into the compact marker selection form. */
[[nodiscard]] bool marker_anchor(const catalog::Snapshot& snapshot,
                                 const BrowserRow& source,
                                 marker::Anchor& output) noexcept {
    output = {};
    output.sourceKind = source.sourceKind;
    output.sourceRow = source.sourceRow;
    output.ownerRow = source.ownerRow;
    if (source.sourceKind == marker::AnchorSource::containerPlacement) {
        if (source.sourceRow >= snapshot.containerPlacements.size()) {
            return false;
        }
        const catalog::ContainerPlacement& placement =
            snapshot.containerPlacements[source.sourceRow];
        output.objectListTag = placement.objectListTag;
        output.classListTag = placement.classListTag;
        output.entryIndex = placement.entryIndex;
        output.position = placement.position;
        if (source.ownerRow < snapshot.containerPlacementOwners.size()) {
            const auto& owner = snapshot.containerPlacementOwners[source.ownerRow];
            for (std::size_t row = 0; row < snapshot.bubbles.size(); ++row) {
                if (catalog::container_placement_owner_applies(owner,
                                                               static_cast<std::uint32_t>(row))) {
                    output.bubbleRow = static_cast<std::uint32_t>(row);
                    output.bubbleIndex = snapshot.bubbles[row].index;
                    break;
                }
            }
        }
        return true;
    }
    if (source.sourceRow >= snapshot.authoredPlacements.size()) {
        return false;
    }
    const catalog::AuthoredPlacement& placement = snapshot.authoredPlacements[source.sourceRow];
    if (!placement_valid(snapshot, placement)) {
        return false;
    }
    const catalog::State& owner = snapshot.states[placement.stateRow];
    const catalog::Bubble& bubble = snapshot.bubbles[placement.bubbleRow];
    output.bubbleRow = placement.bubbleRow;
    output.bubbleIndex = bubble.index;
    output.stateRow = placement.stateRow;
    output.stateEntryTag = owner.entryTag;
    output.sliceSetIndex = owner.sliceSetIndex;
    output.objectListTag = placement.objectListTag;
    output.classListTag = placement.classListTag;
    output.entryIndex = placement.entryIndex;
    output.position = placement.position;
    return true;
}

/** @return True when this marker set contains the exact browser package row. */
[[nodiscard]] bool placement_selected(const marker::State& selected,
                                      const catalog::Snapshot& snapshot,
                                      const server::activity::host::InstanceSnapshot& instance,
                                      const BrowserRow& source) noexcept {
    return marker::contains(
        selected, marker_context(snapshot, instance), source.sourceKind, source.sourceRow);
}

/** Ticks or clears one package row. It never changes what Show draws. */
void toggle_placement(const catalog::Snapshot& snapshot,
                      const server::activity::host::InstanceSnapshot& instance,
                      const BrowserRow& source) noexcept {
    marker::Anchor anchor{};
    if (!marker_anchor(snapshot, source, anchor)) {
        return;
    }
    marker::toggle({marker_context(snapshot, instance), anchor});
}

/** Maps one all-row cursor through the current package-source filter. */
[[nodiscard]] bool
selection_row(const catalog::Snapshot& snapshot, std::size_t cursor, BrowserRow& output) noexcept {
    output = {};
    const bool includesAuthored = g_sourceFilter != 2;
    const std::size_t authoredCount = includesAuthored ? snapshot.authoredPlacements.size() : 0;
    if (cursor < authoredCount) {
        output.sourceKind = marker::AnchorSource::authoredPlacement;
        output.sourceRow = static_cast<std::uint32_t>(cursor);
        return true;
    }
    if (g_sourceFilter == 1) {
        return false;
    }
    const std::size_t placementRow = cursor - authoredCount;
    if (placementRow >= snapshot.containerPlacements.size()) {
        return false;
    }
    const catalog::ContainerPlacement& placement = snapshot.containerPlacements[placementRow];
    output.sourceKind = marker::AnchorSource::containerPlacement;
    output.sourceRow = static_cast<std::uint32_t>(placementRow);
    output.ownerRow = container_owner_row(snapshot, placement, -1);
    return output.ownerRow != catalog::kNoRow;
}

/** Replaces the marker set from visible, all, or camera-nearby package rows. */
void replace_selection(const catalog::Snapshot& snapshot,
                       const server::activity::host::InstanceSnapshot& instance,
                       RowSet source,
                       const teleport::CameraPose* camera,
                       float nearbyRadius,
                       marker::State& selected) noexcept {
    std::array<marker::Anchor, marker::kSelectionCapacity> anchors{};
    std::size_t matched = 0;
    const std::size_t allCount = (g_sourceFilter != 2 ? snapshot.authoredPlacements.size() : 0)
                                 + (g_sourceFilter != 1 ? snapshot.containerPlacements.size() : 0);
    const std::size_t sourceCount = source == RowSet::visible ? g_visible.size() : allCount;
    for (std::size_t cursor = 0; cursor < sourceCount; ++cursor) {
        BrowserRow row{};
        if (source == RowSet::visible) {
            row = g_visible[cursor];
        } else if (!selection_row(snapshot, cursor, row)) {
            continue;
        }
        marker::Anchor anchor{};
        if (!marker_anchor(snapshot, row, anchor)
            || (source == RowSet::nearby
                && (camera == nullptr
                    || !marker::in_radius(anchor, camera->position, nearbyRadius)))) {
            continue;
        }
        if (matched < anchors.size()) {
            anchors[matched] = anchor;
        }
        ++matched;
    }
    const std::size_t retained = (std::min)(matched, anchors.size());
    (void)marker::select_many(marker_context(snapshot, instance),
                              std::span<const marker::Anchor>(anchors.data(), retained),
                              matched > retained);
    selected = marker::snapshot();
}

/** Draws world-glyph, label, and bounded multi-selection controls. */
void draw_controls(const catalog::Snapshot& snapshot,
                   const server::activity::host::InstanceSnapshot& instance,
                   marker::State& selected) noexcept {
    render_controls::draw_options(selected);

    teleport::CameraPose camera{};
    const bool cameraAvailable = teleport::camera_pose(camera);
    if (ImGui::Button("Tick listed")) {
        replace_selection(snapshot, instance, RowSet::visible, nullptr, 0.0F, selected);
    }
    ImGui::SameLine();
    if (ImGui::Button("Tick all")) {
        replace_selection(snapshot, instance, RowSet::all, nullptr, 0.0F, selected);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!cameraAvailable);
    if (ImGui::Button("Tick nearby")) {
        replace_selection(
            snapshot, instance, RowSet::nearby, &camera, selected.options.nearbyRadius, selected);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Ticks positions near the camera.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Untick all")) {
        marker::clear();
        selected = marker::snapshot();
    }

    render_controls::draw_status(marker_context(snapshot, instance), selected);
}

/** Draws the first proved config name for one container row. */
void draw_container_links(const catalog::Snapshot& snapshot,
                          const catalog::ContainerPlacement& placement) noexcept {
    const std::size_t configEnd =
        static_cast<std::size_t>(placement.firstConfig) + placement.configCount;
    if (placement.configCount == 0 || configEnd > snapshot.containerPlacementConfigs.size()) {
        ImGui::TextDisabled("none");
        return;
    }
    ImGui::BeginGroup();
    const catalog::ContainerPlacementConfig& config =
        snapshot.containerPlacementConfigs[placement.firstConfig];
    tag_names::draw(snapshot, config.configNameRow, config.configTag);
    if (placement.configCount > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("+%u cfg", placement.configCount - 1);
    }
    ImGui::EndGroup();
}

/** Draws the filtered package-anchor table without merging its two catalog sources. */
void draw_table(const catalog::Snapshot& snapshot,
                const server::activity::host::InstanceSnapshot& instance,
                marker::State& selected) noexcept {
    if (!ImGui::BeginTable(
            "##authored_anchors", 9, kTableFlags, table_layout::size(g_visible.size()))) {
        return;
    }
    ImGui::TableSetupColumn("source");
    ImGui::TableSetupColumn("bubble");
    ImGui::TableSetupColumn("owner");
    ImGui::TableSetupColumn("list");
    ImGui::TableSetupColumn("entry");
    ImGui::TableSetupColumn("class");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("links");
    ImGui::TableSetupColumn("position");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_visible.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const BrowserRow source = g_visible[static_cast<std::size_t>(visible)];
            ImGui::PushID(static_cast<int>(source.sourceKind));
            ImGui::PushID(static_cast<int>(source.sourceRow));
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(source.sourceKind == marker::AnchorSource::authoredPlacement
                                       ? "scenario"
                                       : "container");
            if (source.sourceKind == marker::AnchorSource::containerPlacement) {
                const catalog::ContainerPlacement& placement =
                    snapshot.containerPlacements[source.sourceRow];
                const catalog::ContainerPlacementList& list =
                    snapshot.containerPlacementLists[placement.listRow];
                const catalog::ContainerPlacementOwner& owner =
                    snapshot.containerPlacementOwners[source.ownerRow];
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("scenario");
                ImGui::TableNextColumn();
                tag_names::draw(snapshot, owner.containerNameRow, owner.containerTag);
                ImGui::TableNextColumn();
                tag_names::draw(snapshot, list.objectListNameRow, list.objectListTag);
                if (list.resourceTag != 0) {
                    ImGui::TextDisabled("resource:");
                    ImGui::SameLine();
                    tag_names::draw(snapshot, list.resourceNameRow, list.resourceTag);
                }
                ImGui::TableNextColumn();
                std::array<char, 24> label{};
                (void)std::snprintf(label.data(), label.size(), "%u", placement.entryIndex);
                const bool rowSelected = placement_selected(selected, snapshot, instance, source);
                if (table_layout::selectable(label.data(), rowSelected)) {
                    toggle_placement(snapshot, instance, source);
                    selected = marker::snapshot();
                }
                ImGui::TableNextColumn();
                tag_names::draw(snapshot, placement.classListNameRow, placement.classListTag);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tables::placed_object_type_name(placement.objectType));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("engine object type %u", placement.objectType);
                }
                ImGui::TableNextColumn();
                draw_container_links(snapshot, placement);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f, %.3f, %.3f",
                            static_cast<double>(placement.position[0]),
                            static_cast<double>(placement.position[1]),
                            static_cast<double>(placement.position[2]));
                ImGui::PopID();
                ImGui::PopID();
                continue;
            }
            const catalog::AuthoredPlacement& placement =
                snapshot.authoredPlacements[source.sourceRow];
            const catalog::Object& object = snapshot.objects[placement.sourceObjectRow];
            const catalog::State& state = snapshot.states[placement.stateRow];
            const catalog::Bubble& bubble = snapshot.bubbles[placement.bubbleRow];
            ImGui::TableNextColumn();
            draw_name(snapshot, bubble.nameRow);
            ImGui::TableNextColumn();
            tag_names::draw(snapshot, object.objectNameRow, object.objectTag);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Where the row came from. It has no ClientRef link.");
            }
            ImGui::TableNextColumn();
            tag_names::draw(snapshot, placement.objectListNameRow, placement.objectListTag);
            ImGui::TableNextColumn();
            std::array<char, 24> label{};
            (void)std::snprintf(label.data(), label.size(), "%u", placement.entryIndex);
            const bool rowSelected = placement_selected(selected, snapshot, instance, source);
            const bool clicked = table_layout::selectable(label.data(), rowSelected);
            if (clicked) {
                toggle_placement(snapshot, instance, source);
                selected = marker::snapshot();
            }
            ImGui::TableNextColumn();
            tag_names::draw(snapshot, placement.classListNameRow, placement.classListTag);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("scenario");
            ImGui::TableNextColumn();
            draw_name(snapshot, state.nameRow);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f, %.3f, %.3f",
                        static_cast<double>(placement.position[0]),
                        static_cast<double>(placement.position[1]),
                        static_cast<double>(placement.position[2]));
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace

/** Draws package positions that no slot claims. */
void draw_positions(const catalog::Snapshot& snapshot,
                    const server::activity::host::InstanceSnapshot& instance,
                    const Filters& filters) noexcept {
    // Combo entries in package-position source order; the index is the stored filter value.
    constexpr std::array<const char*, 3> sources{
        "All package positions", "Scenario-authored", "Container placements"};
    ImGui::SetNextItemWidth(180.0F);
    (void)ImGui::Combo("Source##package_position_filter",
                       &g_sourceFilter,
                       sources.data(),
                       static_cast<int>(sources.size()));
    if (!materialize(snapshot, filters)) {
        ImGui::TextDisabled("Position row storage did not fit");
        marker::publish_no_rows();
        return;
    }
    ImGui::TextDisabled("%zu listed", g_visible.size());
    const catalog::ContainerPlacementDiagnostics& diagnostics =
        snapshot.containerPlacementDiagnostics;
    if (!diagnostics.complete) {
        ImGui::TextDisabled("Position links partial");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("unresolved %llu; dropped %llu",
                              static_cast<unsigned long long>(diagnostics.unresolvedReads),
                              static_cast<unsigned long long>(
                                  diagnostics.droppedLists + diagnostics.droppedOwners
                                  + diagnostics.droppedPlacements + diagnostics.droppedConfigs
                                  + diagnostics.droppedComponents));
        }
    }
    // The catalog scan owns this page's row set, so no explicit list is published and the
    // search cannot reach the renderer.
    (void)marker::publish_rows(marker_context(snapshot, instance), {});
    marker::State selected = marker::snapshot();
    draw_controls(snapshot, instance, selected);
    draw_table(snapshot, instance, selected);
}

} // namespace sunrise::server::ui::activity_host::authored_anchors
