#include "activity_host_scriptable_browser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/content/activity/scriptable_catalog_worker.h"
#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/content/packages/tables/slot_type.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../activity/host_runtime.h"
#include "../../bap/runtime.h"
#include "activity_host_authored_anchors.h"
#include "activity_host_device_probe.h"
#include "activity_host_package_tag_names.h"
#include "activity_host_scriptable_details.h"
#include "activity_host_scriptable_labels.h"
#include "activity_host_scriptable_world_links.h"
#include "activity_host_table_layout.h"
#include "activity_host_trigger_volumes.h"

namespace sunrise::server::ui::activity_host::scriptable_browser {
namespace {

namespace auth = middleware::bap::activity_message::scriptable_auth;
namespace catalog = state::build_data::scriptables;
namespace device_probe = server::ui::activity_host::device_probe;
namespace extraction = client::content::activity::scriptables;
namespace host = server::activity::host;
namespace labels = server::ui::activity_host::scriptable_labels;
namespace marker = client::ui::activity::authored_placement_marker;
namespace scriptable_details = server::ui::activity_host::scriptable_details;
namespace tag_names = server::ui::activity_host::package_tag_names;
namespace trigger_volumes = server::ui::activity_host::trigger_volumes;
namespace scaling = core::ui::scaling::dpi;
namespace tables = middleware::content::packages::tables;

/** One shared table style, so every table on this page reads the same. */
constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

ImGuiTextFilter g_filter{};
std::uint64_t g_selectedRevision{};
std::uint32_t g_selectedObject{catalog::kNoRow};
std::uint32_t g_selectedSlot{catalog::kNoRow};
std::vector<std::uint32_t> g_visibleSlots{};
std::uint64_t g_materializedRevision{};
std::array<char, 256> g_materializedFilter{};
int g_bubbleFilter{-1};
int g_stateFilter{-1};
int g_scopeFilter{};
/** Slot type the current page lists, or a negative value for every type. */
int g_slotTypeFilter{-1};
int g_materializedBubble{-2};
int g_materializedState{-2};
int g_materializedScope{-1};
int g_materializedSlotType{-2};
bool g_materializedUnnamed{};
bool g_showUnnamed{};
bool g_materializedRenderableOnly{};
bool g_renderableOnly{};
int g_cancelResult{};
std::uint64_t g_feedbackSessionId{};
std::uint64_t g_feedbackCreatedRevision{};
bool g_feedbackBound{};

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

/** Copies one extracted name into an ImGui-safe null-terminated label. */
[[nodiscard]] bool selected_label(const catalog::Snapshot& snapshot,
                                  std::uint32_t row,
                                  std::array<char, catalog::kNameCapacity + 1>& output) noexcept {
    output = {};
    const std::string_view name = selected_name(snapshot, row);
    if (name.empty()) {
        return false;
    }
    (void)std::snprintf(
        output.data(), output.size(), "%.*s", static_cast<int>(name.size()), name.data());
    return true;
}

/** Draws a bubble filter containing extracted names only. */
void draw_bubble_filter(const catalog::Snapshot& snapshot) noexcept {
    std::array<char, catalog::kNameCapacity + 1> preview{};
    bool currentNamed = false;
    for (const catalog::Bubble& bubble : snapshot.bubbles) {
        if (g_bubbleFilter >= 0 && bubble.index == static_cast<std::uint32_t>(g_bubbleFilter)) {
            currentNamed = selected_label(snapshot, bubble.nameRow, preview);
            break;
        }
    }
    if (g_bubbleFilter >= 0 && !currentNamed) {
        g_bubbleFilter = -1;
        g_stateFilter = -1;
    }
    if (!ImGui::BeginCombo("Bubble", currentNamed ? preview.data() : "all bubbles")) {
        return;
    }
    if (ImGui::Selectable("all bubbles", g_bubbleFilter < 0)) {
        g_bubbleFilter = -1;
        g_stateFilter = -1;
    }
    for (const catalog::Bubble& bubble : snapshot.bubbles) {
        std::array<char, catalog::kNameCapacity + 1> label{};
        if (!selected_label(snapshot, bubble.nameRow, label)) {
            continue;
        }
        ImGui::PushID(static_cast<int>(bubble.index));
        const bool selected = g_bubbleFilter == static_cast<int>(bubble.index);
        if (ImGui::Selectable(label.data(), selected)) {
            if (!selected) {
                g_stateFilter = -1;
            }
            g_bubbleFilter = static_cast<int>(bubble.index);
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/** Draws named states for the selected bubble and no ambiguous cross-bubble indices. */
void draw_state_filter(const catalog::Snapshot& snapshot) noexcept {
    const catalog::Bubble* selectedBubble = nullptr;
    for (const catalog::Bubble& bubble : snapshot.bubbles) {
        if (g_bubbleFilter >= 0 && bubble.index == static_cast<std::uint32_t>(g_bubbleFilter)) {
            selectedBubble = &bubble;
            break;
        }
    }
    std::array<char, catalog::kNameCapacity + 1> preview{};
    bool currentNamed = false;
    if (selectedBubble != nullptr && g_stateFilter >= 0) {
        const std::size_t end = (std::min)(snapshot.states.size(),
                                           static_cast<std::size_t>(selectedBubble->firstState)
                                               + selectedBubble->stateCount);
        for (std::size_t row = selectedBubble->firstState; row < end; ++row) {
            const catalog::State& state = snapshot.states[row];
            if (state.index == static_cast<std::uint32_t>(g_stateFilter)) {
                currentNamed = selected_label(snapshot, state.nameRow, preview);
                break;
            }
        }
    }
    if (g_stateFilter >= 0 && !currentNamed) {
        g_stateFilter = -1;
    }
    ImGui::BeginDisabled(selectedBubble == nullptr);
    if (ImGui::BeginCombo("State", currentNamed ? preview.data() : "all states")) {
        if (ImGui::Selectable("all states", g_stateFilter < 0)) {
            g_stateFilter = -1;
        }
        const std::size_t end =
            selectedBubble == nullptr
                ? 0
                : (std::min)(snapshot.states.size(),
                             static_cast<std::size_t>(selectedBubble->firstState)
                                 + selectedBubble->stateCount);
        for (std::size_t row = selectedBubble == nullptr ? 0 : selectedBubble->firstState;
             row < end;
             ++row) {
            const catalog::State& state = snapshot.states[row];
            std::array<char, catalog::kNameCapacity + 1> label{};
            if (!selected_label(snapshot, state.nameRow, label)) {
                continue;
            }
            ImGui::PushID(static_cast<int>(state.index));
            const bool selected = g_stateFilter == static_cast<int>(state.index);
            if (ImGui::Selectable(label.data(), selected)) {
                g_stateFilter = static_cast<int>(state.index);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (selectedBubble == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Pick a bubble first.");
    }
}

/** Draws one package name or a short unresolved label. */
void draw_name(const catalog::Snapshot& snapshot, std::uint32_t row) noexcept {
    const catalog::Name* name = name_row(snapshot, row);
    const std::string_view value = selected_name(snapshot, row);
    if (!value.empty()) {
        ImGui::Text("%.*s", static_cast<int>(value.size()), value.data());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", labels::provenance(name->provenance));
        }
    } else if (name != nullptr && name->strongestTierOverflow) {
        ImGui::TextDisabled("Name list incomplete");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Too many names to list.");
        }
    } else if (name != nullptr && name->candidateCount != 0) {
        ImGui::TextDisabled("Ambiguous name");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%u candidates", static_cast<unsigned>(name->candidateCount));
        }
    } else {
        ImGui::TextDisabled("Unnamed");
    }
}

/** @return True when one name row passes the active text filter. */
[[nodiscard]] bool filter_name(const catalog::Snapshot& snapshot, std::uint32_t nameRow) noexcept {
    if (!g_filter.IsActive()) {
        return true;
    }
    const catalog::Name* name = name_row(snapshot, nameRow);
    if (name == nullptr) {
        return false;
    }
    const std::size_t end = static_cast<std::size_t>(name->firstCandidate) + name->candidateCount;
    for (std::size_t index = name->firstCandidate;
         index < end && index < snapshot.nameCandidates.size();
         ++index) {
        const catalog::NameCandidate& candidate = snapshot.nameCandidates[index];
        if (g_filter.PassFilter(candidate.value.data(),
                                candidate.value.data() + candidate.length)) {
            return true;
        }
    }
    return false;
}

/** @return True when one slot passes the active text filter. */
[[nodiscard]] bool slot_matches(const catalog::Snapshot& snapshot,
                                const catalog::Object& object,
                                const catalog::Slot& slot) noexcept {
    if (!g_filter.IsActive() || filter_name(snapshot, slot.nameRow)) {
        return true;
    }
    if (tag_names::matches(snapshot, object.registryNameRow, g_filter)
        || tag_names::matches(snapshot, object.objectNameRow, g_filter)) {
        return true;
    }
    std::array<char, 320> identity{};
    (void)std::snprintf(identity.data(),
                        identity.size(),
                        "0x%08X 0x%08X 0x%08X type %u index %u %s %s bubble %u state %u",
                        static_cast<unsigned>(object.registryKey),
                        static_cast<unsigned>(object.objectTag),
                        static_cast<unsigned>(slot.nameHash),
                        static_cast<unsigned>(slot.slotType),
                        static_cast<unsigned>(slot.slotIndex),
                        labels::scope(object.registryDescriptor),
                        labels::presence(object.safety),
                        static_cast<unsigned>(object.bubbleRow),
                        static_cast<unsigned>(snapshot.states[object.stateRow].index));
    return g_filter.PassFilter(identity.data());
}

[[nodiscard]] bool structural_filter_matches(const catalog::Snapshot& snapshot,
                                             const catalog::Object& object) noexcept {
    const catalog::State& state = snapshot.states[object.stateRow];
    const catalog::Bubble& bubble = snapshot.bubbles[object.bubbleRow];
    return (g_bubbleFilter < 0 || bubble.index == static_cast<std::uint32_t>(g_bubbleFilter))
           && (g_stateFilter < 0 || state.index == static_cast<std::uint32_t>(g_stateFilter))
           && labels::scope_matches(object.registryDescriptor, g_scopeFilter);
}

/** Rebuilds filtered row indices as one commit and preserves the old view on allocation failure. */
[[nodiscard]] bool materialize_visible_rows(const catalog::Snapshot& snapshot) noexcept {
    const bool current =
        g_materializedRevision == snapshot.revision && g_materializedBubble == g_bubbleFilter
        && g_materializedState == g_stateFilter && g_materializedScope == g_scopeFilter
        && g_materializedSlotType == g_slotTypeFilter && g_materializedUnnamed == g_showUnnamed
        && g_materializedRenderableOnly == g_renderableOnly
        && std::strncmp(g_materializedFilter.data(), g_filter.InputBuf, g_materializedFilter.size())
               == 0;
    if (current) {
        return true;
    }
    if (!rebuild_renderable_index(snapshot)) {
        return false;
    }
    try {
        std::vector<std::uint32_t> slots{};
        std::vector<marker::Anchor> listedAnchors{};
        slots.reserve(snapshot.slots.size());
        listedAnchors.reserve(
            (std::min)(renderable_anchor_count(), marker::kRenderSourceVisitCapacity));
        bool listedAnchorsCapped = false;
        for (std::size_t index = 0; index < snapshot.slots.size(); ++index) {
            const catalog::Slot& slot = snapshot.slots[index];
            if (slot.objectRow >= snapshot.objects.size()) {
                continue;
            }
            const catalog::Object& object = snapshot.objects[slot.objectRow];
            if ((g_slotTypeFilter < 0 || slot.slotType == g_slotTypeFilter)
                && (g_showUnnamed || !selected_name(snapshot, slot.nameRow).empty())
                && (!g_renderableOnly || slot_renderable(static_cast<std::uint32_t>(index)))
                && structural_filter_matches(snapshot, object)
                && slot_matches(snapshot, object, slot)) {
                slots.push_back(static_cast<std::uint32_t>(index));
                for (const SlotAnchor& row : slot_anchors(static_cast<std::uint32_t>(index))) {
                    if (listedAnchors.size() == marker::kRenderSourceVisitCapacity) {
                        listedAnchorsCapped = true;
                        break;
                    }
                    listedAnchors.push_back(row.anchor);
                }
            }
        }
        g_visibleSlots.swap(slots);
        swap_listed_anchors(listedAnchors, listedAnchorsCapped);
        g_materializedRevision = snapshot.revision;
        g_materializedBubble = g_bubbleFilter;
        g_materializedState = g_stateFilter;
        g_materializedScope = g_scopeFilter;
        g_materializedSlotType = g_slotTypeFilter;
        g_materializedUnnamed = g_showUnnamed;
        g_materializedRenderableOnly = g_renderableOnly;
        g_materializedFilter = {};
        (void)std::snprintf(
            g_materializedFilter.data(), g_materializedFilter.size(), "%s", g_filter.InputBuf);
        return true;
    } catch (...) {
        return false;
    }
}

/** Selects one exact slot and resets action feedback owned by the prior selection. */
void select_slot(const catalog::Snapshot& snapshot, std::uint32_t slotRow) noexcept {
    g_selectedRevision = snapshot.revision;
    g_selectedSlot = slotRow;
    g_selectedObject =
        slotRow < snapshot.slots.size() ? snapshot.slots[slotRow].objectRow : catalog::kNoRow;
    g_cancelResult = 0;
}

/** Draws one name-first world-object list backed by exact slot rows. */
void draw_world_objects(const catalog::Snapshot& snapshot,
                        const host::InstanceSnapshot& instance,
                        marker::State& selected) noexcept {
    if (!ImGui::BeginTable(
            "##world_objects", 8, kTableFlags, table_layout::size(g_visibleSlots.size()))) {
        return;
    }
    ImGui::TableSetupColumn("Draw");
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Bubble");
    ImGui::TableSetupColumn("State");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Shape");
    ImGui::TableSetupColumn("Link");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_visibleSlots.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const std::uint32_t index = g_visibleSlots[static_cast<std::size_t>(visible)];
            const catalog::Slot& slot = snapshot.slots[index];
            const catalog::Object& object = snapshot.objects[slot.objectRow];
            const catalog::State& state = snapshot.states[object.stateRow];
            const catalog::Bubble& bubble = snapshot.bubbles[object.bubbleRow];
            const std::span<const SlotAnchor> anchors = slot_anchors(index);
            const marker::Context context = marker_context(snapshot, instance);
            ImGui::PushID(static_cast<int>(index));
            table_layout::next_row();
            ImGui::TableNextColumn();
            const std::size_t selectedCount = selected_anchor_count(anchors, context, selected);
            bool rendered = selectedCount != 0;
            ImGui::BeginDisabled(anchors.empty());
            if (ImGui::Checkbox("##render", &rendered)) {
                set_slot_rendering(anchors, context, rendered, selected);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (anchors.empty()) {
                    ImGui::SetTooltip("This row has no place in the world.");
                } else {
                    ImGui::SetTooltip("%zu of %zu places ticked", selectedCount, anchors.size());
                }
            }
            ImGui::TableNextColumn();
            const bool rowSelected =
                g_selectedRevision == snapshot.revision && g_selectedSlot == index;
            const std::string_view name = selected_name(snapshot, slot.nameRow);
            std::array<char, 160> label{};
            if (!name.empty()) {
                (void)std::snprintf(
                    label.data(), label.size(), "%.*s", static_cast<int>(name.size()), name.data());
            } else {
                (void)std::snprintf(label.data(), label.size(), "Unnamed object");
            }
            if (table_layout::selectable(label.data(), rowSelected)) {
                select_slot(snapshot, index);
            }
            if (ImGui::IsItemHovered()) {
                const catalog::Name* row = name_row(snapshot, slot.nameRow);
                ImGui::SetTooltip(
                    "%s", row != nullptr ? labels::provenance(row->provenance) : "unresolved");
            }
            ImGui::TableNextColumn();
            draw_name(snapshot, bubble.nameRow);
            ImGui::TableNextColumn();
            draw_name(snapshot, state.nameRow);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tables::slot_type_name(slot.slotType));
            const WorldSummary summary = world_summary(snapshot, index);
            ImGui::TableNextColumn();
            if (summary.linkedPositions != 0) {
                ImGui::Text("%zu linked", summary.linkedPositions);
            } else if (summary.contextPositions != 0) {
                ImGui::TextDisabled("%zu context", summary.contextPositions);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (summary.shapes != 0) {
                ImGui::Text("%zu volume%s", summary.shapes, summary.shapes == 1 ? "" : "s");
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", link_label(summary));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Offers exact-revision cancellation without depending on the current package selection. */
void draw_pending_override_cancel(const host::InstanceSnapshot& instance) noexcept {
    const bool pending = instance.outputPending
                         && instance.outputKind == host::OutputKind::scriptableOverride
                         && instance.scriptableRevision != 0;
    if (pending) {
        ImGui::SeparatorText("Pending override");
        if (ImGui::Button("Cancel pending")) {
            const bool canceled = server::bap::cancel_activity_scriptable_override(
                instance.binding, instance.scriptableRevision);
            g_cancelResult = canceled ? 1 : -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("not sent yet");
    }
    if (g_cancelResult > 0) {
        ImGui::TextDisabled("canceled before it was sent");
    } else if (g_cancelResult < 0) {
        ImGui::TextDisabled("cancel refused. It was already sent.");
    }
}

void draw_actions(const catalog::Snapshot& snapshot,
                  const host::InstanceSnapshot& instance) noexcept;

/** Draws the selected object's linked world data, actions, and optional identifiers. */
void draw_selection(const catalog::Snapshot& snapshot,
                    const host::InstanceSnapshot& instance) noexcept {
    draw_pending_override_cancel(instance);
    if (g_selectedRevision != snapshot.revision || g_selectedObject >= snapshot.objects.size()
        || g_selectedSlot >= snapshot.slots.size()) {
        ImGui::TextDisabled("Pick a row.");
        return;
    }
    const catalog::Object& object = snapshot.objects[g_selectedObject];
    const catalog::State& owner = snapshot.states[object.stateRow];
    const catalog::Slot& slot = snapshot.slots[g_selectedSlot];
    std::array<char, catalog::kNameCapacity + 1> heading{};
    ImGui::SeparatorText(selected_label(snapshot, slot.nameRow, heading) ? heading.data()
                                                                         : "Selected object");

    const WorldSummary summary = world_summary(snapshot, g_selectedSlot);
    if (ImGui::BeginTable("##selected_world_summary", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Position");
        ImGui::TableSetupColumn("Shape");
        ImGui::TableSetupColumn("Link");
        table_layout::frozen_headers();
        table_layout::next_row();
        ImGui::TableNextColumn();
        if (summary.linkedPositions != 0) {
            ImGui::Text("%zu linked", summary.linkedPositions);
        } else if (summary.contextPositions != 0) {
            ImGui::TextDisabled("%zu context only", summary.contextPositions);
        } else {
            ImGui::TextDisabled("none");
        }
        ImGui::TableNextColumn();
        if (summary.shapes != 0) {
            ImGui::Text("%zu volume%s", summary.shapes, summary.shapes == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("none");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(link_label(summary));
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Position");
    scriptable_details::draw_embedded_placements(snapshot, instance, slot);
    scriptable_details::draw_type23_placements(snapshot, instance, slot);

    ImGui::SeparatorText("Shape");
    trigger_volumes::draw_selected(snapshot, instance, g_selectedSlot);

    ImGui::SeparatorText("Actions");
    draw_actions(snapshot, instance);

    if (ImGui::TreeNodeEx("Technical details##world_object", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Text("object row %u; slot row %u",
                    static_cast<unsigned>(g_selectedObject),
                    static_cast<unsigned>(g_selectedSlot));
        ImGui::Text("key 0x%08X; slot %u; type %u",
                    static_cast<unsigned>(object.registryKey),
                    static_cast<unsigned>(slot.slotIndex),
                    static_cast<unsigned>(slot.slotType));
        ImGui::Text("name hash 0x%08X; %s; %s",
                    static_cast<unsigned>(slot.nameHash),
                    labels::scope(object.registryDescriptor),
                    labels::presence(object.safety));
        ImGui::TextUnformatted("state");
        ImGui::SameLine();
        tag_names::draw(snapshot, owner.entryNameRow, owner.entryTag);
        ImGui::TextUnformatted("registry");
        ImGui::SameLine();
        tag_names::draw(snapshot, object.registryNameRow, object.registryTag);
        ImGui::TextUnformatted("object");
        ImGui::SameLine();
        tag_names::draw(snapshot, object.objectNameRow, object.objectTag);
        scriptable_details::draw_live_instances(snapshot, slot);
        scriptable_details::draw_descriptors(snapshot, slot);
        scriptable_details::draw_references(snapshot, g_selectedObject);
        ImGui::TreePop();
    }
}

/** Draws the list filters shared by every world page. They never change what is drawn. */
void draw_filters(const catalog::Snapshot& snapshot,
                  marker::State& markerState,
                  bool offerPositionFilter) noexcept {
    g_filter.Draw("Search##scriptable_objects", scaling::pixels(360.0F));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filters the list. The world still draws the same rows.");
    }
    if (offerPositionFilter) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Has a place##scriptable_objects", &g_renderableOnly)) {
            marker::Options options = markerState.options;
            options.onlyRenderableObjects = g_renderableOnly;
            marker::set_options(options);
            markerState.options = options;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Only rows with a position or a shape.");
        }
    }
    if (ImGui::TreeNodeEx("More filters##scriptable_objects", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::SetNextItemWidth(scaling::pixels(240.0F));
        draw_bubble_filter(snapshot);
        ImGui::SetNextItemWidth(scaling::pixels(240.0F));
        draw_state_filter(snapshot);
        ImGui::SetNextItemWidth(scaling::pixels(170.0F));
        // Scope combo entries; the index selects the registry descriptor in scope_matches.
        constexpr std::array<const char*, 4> scopes{"all", "shared", "registry", "state-local"};
        (void)ImGui::Combo("Scope", &g_scopeFilter, scopes.data(), static_cast<int>(scopes.size()));
        ImGui::Checkbox("Show unnamed", &g_showUnnamed);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Include rows with no recovered name.");
        }
        ImGui::TreePop();
    }
}

/** Draws the name-first object list and selected-object workspace. */
void draw_browse(const catalog::Snapshot& snapshot,
                 const host::InstanceSnapshot& instance) noexcept {
    if (!materialize_visible_rows(snapshot)) {
        ImGui::TextDisabled("Row storage did not fit");
        marker::publish_no_rows();
        return;
    }
    marker::State selected = marker::snapshot();
    draw_world_render_controls(snapshot, instance, selected);
    ImGui::TextDisabled("%zu listed", g_visibleSlots.size());
    draw_world_objects(snapshot, instance, selected);
    draw_selection(snapshot, instance);
}

/** Draws only SDK-backed actions with a proved typed adapter. */
void draw_actions(const catalog::Snapshot& snapshot,
                  const host::InstanceSnapshot& instance) noexcept {
    if (!device_probe::draw(snapshot, instance, g_selectedObject, g_selectedSlot)) {
        ImGui::TextDisabled("No action for this row.");
    }
}

/** One world page and the slot type it lists. */
enum class Page : std::uint8_t {
    objects,
    devices,
    triggers,
    positions,
};

/** Draws the slot-backed list for one page, then that page's action or monitor block. */
void draw_page(const catalog::Snapshot& snapshot,
               const host::InstanceSnapshot& instance,
               Page page) noexcept {
    if (snapshot.coverage != catalog::BuildCoverage::full) {
        ImGui::TextDisabled("Still reading world data.");
    }
    marker::State markerState = marker::snapshot();
    g_renderableOnly = markerState.options.onlyRenderableObjects;
    const bool slotBacked = page == Page::objects || page == Page::devices;
    g_slotTypeFilter = page == Page::devices ? static_cast<int>(auth::kType23SlotType) : -1;
    switch (page) {
    case Page::objects:
    case Page::devices:
    case Page::triggers:
    case Page::positions:
        // The navigation column already names each page. A second sentence here is noise.
        break;
    }
    draw_filters(snapshot, markerState, slotBacked);
    switch (page) {
    case Page::objects:
    case Page::devices:
        draw_browse(snapshot, instance);
        return;
    case Page::triggers: {
        const authored_anchors::Filters filters{
            &g_filter, g_bubbleFilter, g_stateFilter, g_scopeFilter};
        trigger_volumes::draw(snapshot, instance, filters);
        return;
    }
    case Page::positions: {
        const authored_anchors::Filters filters{
            &g_filter, g_bubbleFilter, g_stateFilter, g_scopeFilter};
        authored_anchors::draw_positions(snapshot, instance, filters);
        return;
    }
    }
}

/** Resolves the selected destination to the package layout used by world extraction. */
[[nodiscard]] bool resolve_layout(const host::InstanceSnapshot* instance,
                                  std::string_view& name,
                                  state::build_data::scenarios::Definition& layout) noexcept {
    if (instance == nullptr) {
        return false;
    }
    const auto& destination = instance->binding.destination;
    name = std::string_view(reinterpret_cast<const char*>(destination.packageName.data()),
                            destination.packageNameLength);
    return state::build_data::find_scenario_layout(name, layout);
}

/** Draws one selected activity's package-derived world page. */
void draw_contents(const host::InstanceSnapshot* instance, Page page) noexcept {
    if (instance == nullptr) {
        g_feedbackBound = false;
        g_cancelResult = 0;
        ImGui::TextDisabled("No activity selected");
        marker::publish_no_rows();
        return;
    }
    if (!g_feedbackBound || g_feedbackSessionId != instance->binding.sessionId
        || g_feedbackCreatedRevision != instance->binding.createdRevision) {
        g_feedbackSessionId = instance->binding.sessionId;
        g_feedbackCreatedRevision = instance->binding.createdRevision;
        g_feedbackBound = true;
        g_cancelResult = 0;
    }
    std::string_view name{};
    state::build_data::scenarios::Definition layout{};
    if (!resolve_layout(instance, name, layout)) {
        ImGui::TextDisabled("No world data for this destination.");
        marker::publish_no_rows();
        return;
    }
    ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) {
        prepare(instance, true);
    }
    const catalog::SnapshotView snapshot = catalog::snapshot();
    if (snapshot == nullptr || snapshot->scenarioTag != layout.tag
        || std::string_view(snapshot->scenarioName.data(), snapshot->scenarioNameLength) != name) {
        ImGui::TextDisabled("World data queued.");
        marker::publish_no_rows();
        return;
    }
    if (snapshot->status == catalog::BuildStatus::ready) {
        marker::show_for_frame();
        draw_page(*snapshot, *instance, page);
    } else {
        ImGui::TextDisabled("%s: %s", labels::status(snapshot->status), snapshot->detail.data());
        marker::publish_no_rows();
    }
}

} // namespace

/** Requests current package-derived world data for one selected activity. */
void prepare(const host::InstanceSnapshot* instance, bool force) noexcept {
    std::string_view name{};
    state::build_data::scenarios::Definition layout{};
    if (!resolve_layout(instance, name, layout)) {
        return;
    }
    (void)extraction::request(layout.tag, name, force);
}

/** Draws named world objects: every slot with a position or a shape. */
void draw_objects(const host::InstanceSnapshot* instance) noexcept {
    draw_contents(instance, Page::objects);
}

/** Draws type-23 device slots and the set-channel action for the highlighted row. */
void draw_devices(const host::InstanceSnapshot* instance) noexcept {
    draw_contents(instance, Page::devices);
}

/** Draws type-60 trigger volumes and their authored prism edges. */
void draw_triggers(const host::InstanceSnapshot* instance) noexcept {
    draw_contents(instance, Page::triggers);
}

/** Draws package positions that no slot claims. */
void draw_positions(const host::InstanceSnapshot* instance) noexcept {
    draw_contents(instance, Page::positions);
}

} // namespace sunrise::server::ui::activity_host::scriptable_browser
