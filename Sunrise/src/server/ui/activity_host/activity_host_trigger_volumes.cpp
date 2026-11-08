#include "activity_host_trigger_volumes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <string_view>
#include <vector>

#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../client/ui/activity/authored_spatial_overlay.h"
#include "../../../client/ui/activity/package_trigger_volume_geometry.h"
#include "../../../client/ui/activity/package_trigger_volume_marker_source.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../activity/host_runtime.h"
#include "activity_host_anchor_render_controls.h"
#include "activity_host_package_tag_names.h"
#include "activity_host_scriptable_labels.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::trigger_volumes {
namespace {

namespace catalog = state::build_data::scriptables;
namespace marker = client::ui::activity::authored_placement_marker;
namespace marker_source = client::ui::activity::package_trigger_volume_marker_source;
namespace overlay = client::ui::activity::authored_spatial_overlay;
namespace render_controls = server::ui::activity_host::anchor_render_controls;
namespace tag_names = server::ui::activity_host::package_tag_names;
namespace labels = server::ui::activity_host::scriptable_labels;

/** One shared table style, so every table on this page reads the same. */
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
/** Listed rows kept before the browser reports it capped; a package holds far fewer. */
constexpr std::size_t kBrowserRowCapacity = 262'144;

/** One owner and one exact root-identity candidate; no-instance tables retain a sentinel row. */
struct BrowserRow final {
    std::uint32_t ownerRow{catalog::kNoRow};
    std::uint32_t instanceRow{catalog::kNoRow};
};

std::vector<BrowserRow> g_visible{};
std::vector<marker::Anchor> g_scenarioAnchors{};
std::vector<marker::Anchor> g_listedAnchors{};
bool g_listedAnchorsCapped{};
std::uint64_t g_materializedRevision{};
std::array<char, 256> g_materializedFilter{};
int g_materializedBubble{-2};
int g_materializedState{-2};
int g_materializedScope{-1};
std::size_t g_sourceRowsVisited{};
bool g_browserCapped{};

/** @return One uniquely selected strongest hash name, or an empty view. */
[[nodiscard]] std::string_view selected_hash_name(const catalog::Snapshot& snapshot,
                                                  std::uint32_t row) noexcept {
    if (row >= snapshot.names.size()) {
        return {};
    }
    const catalog::Name& name = snapshot.names[row];
    if (name.selectedCandidate >= snapshot.nameCandidates.size()) {
        return {};
    }
    const catalog::NameCandidate& candidate = snapshot.nameCandidates[name.selectedCandidate];
    return {candidate.value.data(), candidate.length};
}

/** @return One uniquely selected package tag name, or an empty view. */
[[nodiscard]] std::string_view selected_tag_name(const catalog::Snapshot& snapshot,
                                                 std::uint32_t row) noexcept {
    if (row >= snapshot.tagNames.size()) {
        return {};
    }
    const catalog::TagName& name = snapshot.tagNames[row];
    if (name.selectedCandidate >= snapshot.nameCandidates.size()) {
        return {};
    }
    const catalog::NameCandidate& candidate = snapshot.nameCandidates[name.selectedCandidate];
    return {candidate.value.data(), candidate.length};
}

/** @return True when any retained hash-name candidate passes the text filter. */
[[nodiscard]] bool hash_name_matches(const catalog::Snapshot& snapshot,
                                     std::uint32_t row,
                                     const ImGuiTextFilter& filter) noexcept {
    if (row >= snapshot.names.size()) {
        return false;
    }
    const catalog::Name& name = snapshot.names[row];
    const std::size_t first = name.firstCandidate;
    const std::size_t end = (std::min)(first + name.candidateCount, snapshot.nameCandidates.size());
    for (std::size_t candidateRow = first; candidateRow < end; ++candidateRow) {
        const catalog::NameCandidate& candidate = snapshot.nameCandidates[candidateRow];
        if (filter.PassFilter(candidate.value.data(), candidate.value.data() + candidate.length)) {
            return true;
        }
    }
    return false;
}

/** @return True when one owner passes bubble, state, and package-identity filters. */
[[nodiscard]] bool row_matches(const catalog::Snapshot& snapshot,
                               const BrowserRow& row,
                               const authored_anchors::Filters& filters) noexcept {
    if (row.ownerRow >= snapshot.triggerVolumeOwners.size()) {
        return false;
    }
    const catalog::TriggerVolumeOwner& owner = snapshot.triggerVolumeOwners[row.ownerRow];
    if (owner.tableRow >= snapshot.triggerVolumeTables.size()
        || owner.objectRow >= snapshot.objects.size()) {
        return false;
    }
    const catalog::Object& object = snapshot.objects[owner.objectRow];
    if (object.bubbleRow >= snapshot.bubbles.size() || object.stateRow >= snapshot.states.size()
        || (filters.bubbleIndex >= 0
            && snapshot.bubbles[object.bubbleRow].index
                   != static_cast<std::uint32_t>(filters.bubbleIndex))
        || (filters.stateIndex >= 0
            && snapshot.states[object.stateRow].index
                   != static_cast<std::uint32_t>(filters.stateIndex))
        || !labels::scope_matches(object.registryDescriptor, filters.scope)) {
        return false;
    }
    if (filters.text == nullptr || !filters.text->IsActive()) {
        return true;
    }
    const ImGuiTextFilter& filter = *filters.text;
    const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
    const catalog::Slot* const slot =
        owner.slotRow < snapshot.slots.size() ? &snapshot.slots[owner.slotRow] : nullptr;
    const catalog::TriggerVolumeInstance* const instance =
        row.instanceRow < snapshot.triggerVolumeInstances.size()
            ? &snapshot.triggerVolumeInstances[row.instanceRow]
            : nullptr;
    bool incomingNameMatches = false;
    const std::size_t incomingFirst = owner.firstIncomingReference;
    const std::size_t incomingCount = owner.incomingReferenceCount;
    if (incomingFirst <= snapshot.triggerVolumeIncomingReferences.size()
        && incomingCount <= snapshot.triggerVolumeIncomingReferences.size() - incomingFirst) {
        for (std::size_t offset = 0; offset < incomingCount && !incomingNameMatches; ++offset) {
            const auto& incoming = snapshot.triggerVolumeIncomingReferences[incomingFirst + offset];
            if (incoming.sourceSlotRow < snapshot.slots.size()) {
                incomingNameMatches = hash_name_matches(
                    snapshot, snapshot.slots[incoming.sourceSlotRow].nameRow, filter);
            }
        }
    }
    if ((slot != nullptr && hash_name_matches(snapshot, slot->nameRow, filter))
        || incomingNameMatches || tag_names::matches(snapshot, table.configNameRow, filter)
        || tag_names::matches(snapshot, object.objectNameRow, filter)
        || tag_names::matches(snapshot, object.registryNameRow, filter)
        || (instance != nullptr
            && (tag_names::matches(snapshot, instance->classDefinitionNameRow, filter)
                || tag_names::matches(snapshot, instance->shapeResourceNameRow, filter)))) {
        return true;
    }
    std::array<char, 384> identity{};
    (void)std::snprintf(identity.data(),
                        identity.size(),
                        "package trigger volume key 0x%08X type %u index %u config 0x%08X "
                        "object 0x%08X slot row %u owner row %u authored row %u shape 0x%08X "
                        "shape index %u",
                        table.registryKey,
                        static_cast<unsigned>(table.slotType),
                        static_cast<unsigned>(table.slotIndex),
                        table.configTag,
                        object.objectTag,
                        owner.slotRow,
                        row.ownerRow,
                        instance != nullptr ? instance->authoredRowIndex : catalog::kNoRow,
                        instance != nullptr ? instance->shapeResourceTag : 0,
                        instance != nullptr ? instance->shapeIndex : 0);
    return filter.PassFilter(identity.data());
}

/** Rebuilds the bounded owner/candidate browser rows for one catalog and filter generation. */
[[nodiscard]] bool materialize(const catalog::Snapshot& snapshot,
                               const authored_anchors::Filters& filters) noexcept {
    const char* const text = filters.text != nullptr ? filters.text->InputBuf : "";
    if (g_materializedRevision == snapshot.revision && g_materializedBubble == filters.bubbleIndex
        && g_materializedState == filters.stateIndex && g_materializedScope == filters.scope
        && std::strncmp(g_materializedFilter.data(), text, g_materializedFilter.size()) == 0) {
        return true;
    }
    try {
        std::vector<BrowserRow> visible{};
        visible.reserve((std::min)(snapshot.triggerVolumeOwners.size(), kBrowserRowCapacity));
        std::vector<marker::Anchor> scenarioAnchors{};
        std::vector<marker::Anchor> listedAnchors{};
        std::size_t visited = 0;
        bool capped = false;
        bool listedCapped = false;
        for (std::size_t ownerRow = 0; ownerRow < snapshot.triggerVolumeOwners.size(); ++ownerRow) {
            const catalog::TriggerVolumeOwner& owner = snapshot.triggerVolumeOwners[ownerRow];
            if (owner.tableRow >= snapshot.triggerVolumeTables.size()) {
                continue;
            }
            const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
            const std::size_t first = table.firstInstance;
            const std::size_t count = table.instanceCount;
            if (first > snapshot.triggerVolumeInstances.size()
                || count > snapshot.triggerVolumeInstances.size() - first) {
                continue;
            }
            const std::size_t browserCount = count == 0 ? 1 : count;
            for (std::size_t offset = 0; offset < browserCount; ++offset) {
                if (visited == kBrowserRowCapacity) {
                    capped = true;
                    break;
                }
                ++visited;
                const BrowserRow row{static_cast<std::uint32_t>(ownerRow),
                                     count == 0 ? catalog::kNoRow
                                                : static_cast<std::uint32_t>(first + offset)};
                // The scenario set is what the world can draw, so the filters never touch it.
                marker::Anchor anchor{};
                const bool renderable =
                    row.instanceRow != catalog::kNoRow
                    && marker_source::build(snapshot, row.ownerRow, row.instanceRow, anchor);
                if (renderable && scenarioAnchors.size() < marker::kRenderSourceVisitCapacity) {
                    scenarioAnchors.push_back(anchor);
                }
                if (!row_matches(snapshot, row, filters)) {
                    continue;
                }
                visible.push_back(row);
                if (!renderable) {
                    continue;
                }
                if (listedAnchors.size() == marker::kRenderSourceVisitCapacity) {
                    listedCapped = true;
                    continue;
                }
                listedAnchors.push_back(anchor);
            }
            if (capped) {
                break;
            }
        }
        g_visible.swap(visible);
        g_scenarioAnchors.swap(scenarioAnchors);
        g_listedAnchors.swap(listedAnchors);
        g_listedAnchorsCapped = listedCapped;
        g_sourceRowsVisited = visited;
        g_browserCapped = capped;
        g_materializedRevision = snapshot.revision;
        g_materializedBubble = filters.bubbleIndex;
        g_materializedState = filters.stateIndex;
        g_materializedScope = filters.scope;
        g_materializedFilter = {};
        (void)std::snprintf(g_materializedFilter.data(), g_materializedFilter.size(), "%s", text);
        return true;
    } catch (...) {
        return false;
    }
}

/** Builds the exact activity/catalog context shared by manual trigger selections. */
[[nodiscard]] marker::Context
marker_context(const catalog::Snapshot& snapshot,
               const server::activity::host::InstanceSnapshot& instance) noexcept {
    return {instance.binding, snapshot.revision, snapshot.scenarioTag};
}

/** Publishes every drawable shape in this scenario and draws the shared render controls. */
void draw_world_render_controls(const marker::Context& context, marker::State& selected) noexcept {
    ImGui::PushID("trigger_volume_render_controls");
    render_controls::draw_options(selected);
    const bool published =
        marker::publish_rows(context, g_scenarioAnchors, marker::PublishedSource::explicitRows);
    if (ImGui::Button("Tick listed")) {
        (void)marker::select_many(context, g_listedAnchors, g_listedAnchorsCapped);
        selected = marker::snapshot();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ticks every shape owned by a listed row.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Untick all")) {
        marker::clear();
        selected = marker::snapshot();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu drawable in this scenario", g_scenarioAnchors.size());
    if (!published) {
        ImGui::TextDisabled("Draw source unavailable");
    } else if (g_listedAnchorsCapped) {
        ImGui::TextDisabled("Tick listed stops at %zu rows", marker::kRenderSourceVisitCapacity);
    }
    render_controls::draw_status(context, selected);
    ImGui::PopID();
}

/** @return True when one retained incoming trigger alias uses the given source slot. */
[[nodiscard]] bool incoming_slot_matches(const catalog::Snapshot& snapshot,
                                         const catalog::TriggerVolumeOwner& owner,
                                         std::uint32_t slotRow) noexcept {
    const std::size_t first = owner.firstIncomingReference;
    const std::size_t count = owner.incomingReferenceCount;
    if (first > snapshot.triggerVolumeIncomingReferences.size()
        || count > snapshot.triggerVolumeIncomingReferences.size() - first) {
        return false;
    }
    const auto begin =
        snapshot.triggerVolumeIncomingReferences.begin() + static_cast<std::ptrdiff_t>(first);
    return std::any_of(begin,
                       begin + static_cast<std::ptrdiff_t>(count),
                       [slotRow](const catalog::TriggerVolumeIncomingReference& incoming) noexcept {
                           return incoming.sourceSlotRow == slotRow;
                       });
}

/** Draws the strongest slot alias, then falls back to package-authored tag names. */
void draw_trigger_names(const catalog::Snapshot& snapshot,
                        const catalog::TriggerVolumeOwner& owner,
                        const catalog::TriggerVolumeTable& table,
                        const catalog::Object& object,
                        const catalog::TriggerVolumeInstance* instance,
                        const catalog::Slot* fallback) noexcept {
    const std::size_t first = owner.firstIncomingReference;
    const std::size_t count = owner.incomingReferenceCount;
    bool drew = false;
    if (first <= snapshot.triggerVolumeIncomingReferences.size()
        && count <= snapshot.triggerVolumeIncomingReferences.size() - first) {
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& incoming = snapshot.triggerVolumeIncomingReferences[first + offset];
            if (incoming.sourceSlotRow >= snapshot.slots.size()) {
                continue;
            }
            const catalog::Slot& slot = snapshot.slots[incoming.sourceSlotRow];
            const std::string_view name = selected_hash_name(snapshot, slot.nameRow);
            if (name.empty()) {
                continue;
            }
            if (drew) {
                ImGui::SameLine(0.0F, 0.0F);
                ImGui::TextUnformatted(", ");
                ImGui::SameLine(0.0F, 0.0F);
            }
            ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
            drew = true;
        }
    }
    if (!drew && fallback != nullptr) {
        const std::string_view name = selected_hash_name(snapshot, fallback->nameRow);
        if (!name.empty()) {
            ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
            drew = true;
        }
    }
    if (!drew) {
        const std::array<std::uint32_t, 5> rows{
            table.configNameRow,
            object.objectNameRow,
            object.registryNameRow,
            instance != nullptr ? instance->classDefinitionNameRow : catalog::kNoRow,
            instance != nullptr ? instance->shapeResourceNameRow : catalog::kNoRow};
        for (const std::uint32_t row : rows) {
            const std::string_view name = selected_tag_name(snapshot, row);
            if (!name.empty()) {
                ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
                drew = true;
                break;
            }
        }
    }
    if (!drew) {
        // The key and slot index match the SDK's slot declarations and the host logs.
        ImGui::TextDisabled(
            "Unnamed trigger 0x%08X/%u", table.registryKey, static_cast<unsigned>(table.slotIndex));
    }
}

/** Draws exact package, row, bounds, and reference provenance on demand. */
void draw_provenance(const catalog::Snapshot& snapshot,
                     const BrowserRow& row,
                     const catalog::TriggerVolumeOwner& owner,
                     const catalog::TriggerVolumeTable& table,
                     const catalog::TriggerVolumeInstance* instance) noexcept {
    if (!ImGui::IsItemHovered()) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("package trigger volume");
    ImGui::Text("owner %u / object %u / table %u", row.ownerRow, owner.objectRow, owner.tableRow);
    ImGui::Text("key 0x%08X / type %u / index %u / slot row %u",
                table.registryKey,
                static_cast<unsigned>(table.slotType),
                static_cast<unsigned>(table.slotIndex),
                owner.slotRow);
    ImGui::Text("config 0x%08X / component %u / identity matches %u",
                table.configTag,
                table.componentOrdinal,
                table.identityMatchCount);
    ImGui::Text("incoming type-31 and type-30 references: %u", owner.incomingReferenceMatchCount);
    if (owner.incomingReferenceCount == 1
        && owner.firstIncomingReference < snapshot.triggerVolumeIncomingReferences.size()) {
        const catalog::TriggerVolumeIncomingReference& incoming =
            snapshot.triggerVolumeIncomingReferences[owner.firstIncomingReference];
        ImGui::Text("incoming reference row %u / source object %u / source slot %u",
                    incoming.referenceRow,
                    incoming.sourceObjectRow,
                    incoming.sourceSlotRow);
    }
    if (instance != nullptr) {
        ImGui::Separator();
        ImGui::Text("authored row %u / class 0x%08X / flags 0x%08X / active %u",
                    instance->authoredRowIndex,
                    instance->classDefinitionTag,
                    instance->flags,
                    static_cast<unsigned>(instance->active));
        ImGui::Text("shape 0x%08X / index %u / ref 0x%08X",
                    instance->shapeResourceTag,
                    instance->shapeIndex,
                    instance->shapeReferenceWord);
        ImGui::Text("%u vertices / %u triangles / +Z %.3f",
                    instance->vertexCount,
                    instance->triangleCount,
                    static_cast<double>(instance->extrusion));
        ImGui::Text("world bounds %.3f, %.3f, %.3f -> %.3f, %.3f, %.3f",
                    static_cast<double>(instance->minimum[0]),
                    static_cast<double>(instance->minimum[1]),
                    static_cast<double>(instance->minimum[2]),
                    static_cast<double>(instance->maximum[0]),
                    static_cast<double>(instance->maximum[1]),
                    static_cast<double>(instance->maximum[2]));
        ImGui::TextDisabled("needs the exact SpawnEntry transform to draw");
    }
    ImGui::EndTooltip();
}

/** Draws one compact clipped owner/candidate row and its manual selection toggle. */
void draw_row(const catalog::Snapshot& snapshot,
              const server::activity::host::InstanceSnapshot& activity,
              marker::State& selected,
              const BrowserRow& row) noexcept {
    const catalog::TriggerVolumeOwner& owner = snapshot.triggerVolumeOwners[row.ownerRow];
    const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
    const catalog::Object& object = snapshot.objects[owner.objectRow];
    const catalog::Slot* const slot =
        owner.slotRow < snapshot.slots.size() ? &snapshot.slots[owner.slotRow] : nullptr;
    const catalog::TriggerVolumeInstance* const instance =
        row.instanceRow < snapshot.triggerVolumeInstances.size()
            ? &snapshot.triggerVolumeInstances[row.instanceRow]
            : nullptr;
    marker::Anchor anchor{};
    const bool renderable =
        instance != nullptr
        && marker_source::build(snapshot, row.ownerRow, row.instanceRow, anchor);
    const marker::Context context = marker_context(snapshot, activity);
    bool checked = renderable
                   && marker::contains(selected,
                                       context,
                                       marker::AnchorSource::packageTriggerVolume,
                                       row.instanceRow,
                                       row.ownerRow);

    ImGui::PushID(static_cast<int>(row.ownerRow));
    ImGui::PushID(static_cast<int>(row.instanceRow));
    table_layout::next_row();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!renderable);
    if (ImGui::Checkbox("##selected", &checked)) {
        marker::toggle({context, anchor});
        selected = marker::snapshot();
    }
    ImGui::EndDisabled();
    ImGui::TableNextColumn();
    draw_trigger_names(snapshot, owner, table, object, instance, slot);
    ImGui::TableNextColumn();
    if (instance != nullptr) {
        tag_names::draw(snapshot, instance->shapeResourceNameRow, instance->shapeResourceTag);
    } else {
        ImGui::TextDisabled("shape unresolved");
    }
    ImGui::TableNextColumn();
    std::array<char, 64> status{};
    if (renderable) {
        (void)std::snprintf(status.data(), status.size(), "ready");
    } else if (table.identityMatchCount == 0) {
        (void)std::snprintf(status.data(), status.size(), "0 matches");
    } else if (table.identityMatchCount > 1) {
        (void)std::snprintf(status.data(), status.size(), "%u matches", table.identityMatchCount);
    } else if (!table.complete || instance == nullptr || !instance->complete) {
        (void)std::snprintf(status.data(), status.size(), "incomplete");
    } else if (instance->active == 0) {
        (void)std::snprintf(status.data(), status.size(), "inactive");
    } else if (!client::ui::activity::package_trigger_volume_geometry::supported_transform(
                   *instance)) {
        (void)std::snprintf(status.data(), status.size(), "transform unsupported");
    } else {
        (void)std::snprintf(status.data(), status.size(), "owner unresolved");
    }
    ImGui::TextDisabled("%s", status.data());
    draw_provenance(snapshot, row, owner, table, instance);
    ImGui::PopID();
    ImGui::PopID();
}

/** Draws every filtered trigger row through a bounded clipped table. */
void draw_table(const catalog::Snapshot& snapshot,
                const server::activity::host::InstanceSnapshot& instance,
                marker::State& selected) noexcept {
    if (!ImGui::BeginTable(
            "##package_trigger_volumes", 4, kTableFlags, table_layout::size(g_visible.size()))) {
        return;
    }
    ImGui::TableSetupColumn("Draw");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("shape");
    ImGui::TableSetupColumn("status");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_visible.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            draw_row(snapshot, instance, selected, g_visible[static_cast<std::size_t>(visible)]);
        }
    }
    ImGui::EndTable();
}

} // namespace

/** Draws the exact slot-owned package trigger-volume browser and selected-only controls. */
void draw(const catalog::Snapshot& snapshot,
          const server::activity::host::InstanceSnapshot& instance,
          const authored_anchors::Filters& filters) noexcept {
    if (!materialize(snapshot, filters)) {
        ImGui::TextDisabled("Trigger row storage did not fit");
        return;
    }
    ImGui::TextDisabled("%zu listed", g_visible.size());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%zu rows checked", g_sourceRowsVisited);
    }
    if (g_browserCapped) {
        ImGui::TextDisabled("scan stopped at %zu rows", kBrowserRowCapacity);
    }
    const catalog::TriggerVolumeDiagnostics& build = snapshot.triggerVolumeDiagnostics;
    if (!build.complete) {
        ImGui::TextDisabled("Some trigger links are incomplete");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("unresolved %llu; dropped %llu",
                              static_cast<unsigned long long>(build.unresolvedReads),
                              static_cast<unsigned long long>(
                                  build.droppedTables + build.droppedOwners + build.droppedInstances
                                  + build.droppedVertices + build.droppedTriangles
                                  + build.droppedIncomingReferences));
        }
    }
    marker::State selected = marker::snapshot();
    draw_world_render_controls(marker_context(snapshot, instance), selected);
    const overlay::Diagnostics render = overlay::diagnostics();
    if (render.invalidVolumes != 0 || render.edgeCapacityExceeded) {
        ImGui::TextDisabled("last draw: %zu volumes / %zu edges; invalid %zu%s",
                            render.volumes,
                            render.edges,
                            render.invalidVolumes,
                            render.edgeCapacityExceeded ? "; edge cap reached" : "");
    }
    draw_table(snapshot, instance, selected);
}

/** Draws shapes linked to one selected slot. */
void draw_selected(const catalog::Snapshot& snapshot,
                   const server::activity::host::InstanceSnapshot& instance,
                   std::uint32_t slotRow) noexcept {
    std::size_t rowCount = 0;
    for (const catalog::TriggerVolumeOwner& owner : snapshot.triggerVolumeOwners) {
        if ((owner.slotRow != slotRow && !incoming_slot_matches(snapshot, owner, slotRow))
            || owner.tableRow >= snapshot.triggerVolumeTables.size()) {
            continue;
        }
        const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
        rowCount += table.instanceCount != 0 ? table.instanceCount : 1U;
    }
    if (rowCount == 0) {
        ImGui::TextDisabled("No linked shape");
        return;
    }

    // The parent World objects page owns the shared display scope. Merely expanding a selected
    // trigger must not reset Visible Rows or Within radius back to selected-only every frame.
    marker::State selected = marker::snapshot();
    if (!ImGui::BeginTable(
            "##selected_trigger_volumes", 4, kTableFlags, table_layout::size(rowCount, 4))) {
        return;
    }
    ImGui::TableSetupColumn("Draw");
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Shape");
    ImGui::TableSetupColumn("Link");
    table_layout::frozen_headers();
    for (std::size_t ownerRow = 0; ownerRow < snapshot.triggerVolumeOwners.size(); ++ownerRow) {
        const catalog::TriggerVolumeOwner& owner = snapshot.triggerVolumeOwners[ownerRow];
        if ((owner.slotRow != slotRow && !incoming_slot_matches(snapshot, owner, slotRow))
            || owner.tableRow >= snapshot.triggerVolumeTables.size()) {
            continue;
        }
        const catalog::TriggerVolumeTable& table = snapshot.triggerVolumeTables[owner.tableRow];
        if (table.instanceCount == 0) {
            draw_row(snapshot,
                     instance,
                     selected,
                     {static_cast<std::uint32_t>(ownerRow), catalog::kNoRow});
            continue;
        }
        const std::size_t first = table.firstInstance;
        const std::size_t count = table.instanceCount;
        if (first > snapshot.triggerVolumeInstances.size()
            || count > snapshot.triggerVolumeInstances.size() - first) {
            draw_row(snapshot,
                     instance,
                     selected,
                     {static_cast<std::uint32_t>(ownerRow), catalog::kNoRow});
            continue;
        }
        for (std::size_t offset = 0; offset < count; ++offset) {
            draw_row(
                snapshot,
                instance,
                selected,
                {static_cast<std::uint32_t>(ownerRow), static_cast<std::uint32_t>(first + offset)});
        }
    }
    ImGui::EndTable();
}

} // namespace sunrise::server::ui::activity_host::trigger_volumes
