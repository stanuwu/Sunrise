#pragma once

#include <imgui.h>

#include "../../activity/mission_launch_options.h"
#include "mission_launch_lists.h"
#include "mission_launch_model.h"

namespace sunrise::client::ui::mission_launch::manual {
namespace options = client::activity::mission_launch;
namespace picker = lists;
namespace forced = state::activity::forced;

// A local draft, never the published override state.
inline picker::Lists g_lists{};
inline options::ManualScratch g_validation{};
inline forced::ForcedDestination g_value{};
inline bool g_enabled{};
inline std::uint16_t g_detailIndex{0xFFFF}, g_transport{0xFFFF};
inline std::size_t g_revision{};
inline std::array<char, 96> g_activitySearch{};

/** Restarts the draft on one destination and rebuilds its dependent rows. */
inline void select_destination(std::string_view name) noexcept {
    g_value = {};
    if (name.size() > g_value.packageName.size()) {
        name = {};
    }
    std::copy(name.begin(), name.end(), g_value.packageName.begin());
    g_value.packageNameLength = static_cast<std::uint8_t>(name.size());
    g_value.enabled = true;
    picker::refresh_destination(g_lists, name);
    g_revision = state::build_data::scenario_layout_count();
}

inline void select_transport(std::uint16_t index) noexcept {
    g_transport = index;
    const auto rows = state::build_data::activities::entries();
    g_value.hasActivityIndex =
        index < rows.size() && rows[index].name() == options::destination_name(g_value);
    g_value.activityIndex = g_value.hasActivityIndex ? index : 0;
}

/** Picks a bubble, drops the dependent slice and spawn picks, and defaults the slice. */
inline void select_bubble(std::uint8_t bubble) noexcept {
    g_value.hasBubble = true;
    g_value.bubble = bubble;
    g_value.hasSliceSet = false;
    g_value.sliceSet = 0;
    g_value.hasSpawnSetHash = false;
    g_value.spawnSetHash = 0;
    picker::refresh_bubble(g_lists, bubble);
    if (g_lists.sliceCount != 0) {
        g_value.hasSliceSet = true;
        g_value.sliceSet = g_lists.sliceValues[0];
    }
}

inline void follow_activity(const Activity& row) noexcept {
    if (g_detailIndex == row.index && g_revision == state::build_data::scenario_layout_count()) {
        return;
    }
    g_enabled = false;
    g_detailIndex = row.index;
    g_transport = row.index;
    select_destination(row.name());
    select_transport(row.index);
}

/** Prefer an available named definition from the selected destination's own catalog rows. */
inline std::uint16_t default_transport(std::string_view name) noexcept {
    const auto rows = state::build_data::activities::entries();
    std::uint16_t best = 0xFFFF;
    for (const auto& row : rows) {
        if (row.name() == name && content_visible(content_group(row))
            && (best == 0xFFFF
                || representative_score(row, true) > representative_score(rows[best], true))) {
            best = row.index;
        }
    }
    return best;
}

inline void start_custom() noexcept {
    g_enabled = true;
    g_detailIndex = 0xFFFF;
    g_transport = 0xFFFF;
    g_activitySearch.fill(0);
    select_destination({});
}

/** Draws the destination picker of the custom launch page. */
inline void custom_activity() noexcept {
    picker::refresh_activities(g_lists);
    if (g_revision != state::build_data::scenario_layout_count()) {
        const auto previous = g_value.packageName;
        const auto length = g_value.packageNameLength;
        select_destination(
            {previous.data(), length}); // Discard dependent values when source layouts change.
        select_transport(default_transport(options::destination_name(g_value)));
    }
    ImGui::TextUnformatted("Activity");
    ImGui::SetNextItemWidth(-1.0F);
    const auto name = options::destination_name(g_value);
    std::array<char, 41> preview{};
    std::copy(name.begin(), name.end(), preview.begin());
    if (ImGui::BeginCombo("##custom_activity",
                          name.empty() ? "Choose an installed activity..." : preview.data())) {
        ImGui::SetNextItemWidth(-1.0F);
        (void)ImGui::InputTextWithHint("##custom_activity_filter",
                                       "Search activity package...",
                                       g_activitySearch.data(),
                                       g_activitySearch.size());
        for (std::size_t i = 0; i < g_lists.activityCount; ++i) {
            const std::string_view candidate(g_lists.activities[i].data());
            if (!contains(candidate, g_activitySearch.data())) {
                continue;
            }
            if (ImGui::Selectable(g_lists.activities[i].data(), candidate == name)) {
                select_destination(candidate);
                select_transport(default_transport(candidate));
            }
        }
        ImGui::EndCombo();
    }
}

/** Draws the launch-route picker: the catalog rows whose package is the drafted destination. */
inline void transport_picker(std::span<const Activity> rows,
                             std::span<const bool> available) noexcept {
    ImGui::TextUnformatted("Activity variant");
    ImGui::SetNextItemWidth(-1.0F);
    std::array<char, 240> preview{};
    if (g_transport < rows.size()) {
        (void)std::snprintf(preview.data(),
                            preview.size(),
                            "#%u | %s",
                            g_transport,
                            title(rows[g_transport]).data());
    }
    if (ImGui::BeginCombo("##custom_transport",
                          preview[0] ? preview.data() : "Choose the native launch route...")) {
        for (const auto& row : rows) {
            if (row.index >= available.size() || !available[row.index]
                || !content_visible(content_group(row))
                || row.name() != options::destination_name(g_value)) {
                continue;
            }
            std::array<char, 240> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "#%u | %s | %s",
                                row.index,
                                title(row).data(),
                                row.package.data());
            if (ImGui::Selectable(label.data(), g_transport == row.index)) {
                select_transport(row.index);
            }
        }
        ImGui::EndCombo();
    }
}

/** Draws the bubble, slice and spawn pickers for the drafted destination. */
inline void arrival_fields() noexcept {
    if (!g_enabled) {
        return;
    }
    if (g_lists.selected.bubbleCount == 0) {
        ImGui::TextWrapped(
            "No manual arrival is available for this activity. Use its normal launch.");
        return;
    }
    const int columns =
        ImGui::GetContentRegionAvail().x >= 460.0F * ImGui::GetFontSize() / 16.0F ? 2 : 1;
    const bool table =
        ImGui::BeginTable("##manual_arrival", columns, ImGuiTableFlags_SizingStretchSame);
    if (table) {
        ImGui::TableNextColumn();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bubble");
    ImGui::SetNextItemWidth(-1.0F);
    const char* bubblePreview = "Choose a bubble...";
    std::array<char, 48> bubbleValue{};
    for (std::size_t i = 0; i < g_lists.bubbleCount; ++i) {
        if (g_value.hasBubble && g_lists.bubbleOrdinals[i] == g_value.bubble) {
            (void)std::snprintf(bubbleValue.data(), bubbleValue.size(), "%u", g_value.bubble);
            bubblePreview = bubbleValue.data();
        }
    }
    if (ImGui::BeginCombo("##manual_bubble", bubblePreview)) {
        for (std::size_t i = 0; i < g_lists.bubbleCount; ++i) {
            if (ImGui::Selectable(g_lists.bubbles[i].data(),
                                  g_value.hasBubble
                                      && g_value.bubble == g_lists.bubbleOrdinals[i])) {
                select_bubble(g_lists.bubbleOrdinals[i]);
            }
        }
        ImGui::EndCombo();
    }
    if (table) {
        ImGui::TableNextColumn();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Slice");
    ImGui::SetNextItemWidth(-1.0F);
    const char* slicePreview = "Choose a bubble first...";
    std::array<char, 48> sliceValue{};
    for (std::size_t i = 0; i < g_lists.sliceCount; ++i) {
        if (g_value.hasSliceSet && g_value.sliceSet == g_lists.sliceValues[i]) {
            (void)std::snprintf(sliceValue.data(), sliceValue.size(), "%u", g_value.sliceSet);
            slicePreview = sliceValue.data();
        }
    }
    if (ImGui::BeginCombo("##manual_slice", slicePreview)) {
        for (std::size_t i = 0; i < g_lists.sliceCount; ++i) {
            if (ImGui::Selectable(g_lists.slices[i].data(),
                                  g_value.hasSliceSet
                                      && g_value.sliceSet == g_lists.sliceValues[i])) {
                g_value.hasSliceSet = true;
                g_value.sliceSet = g_lists.sliceValues[i];
                g_value.hasSpawnSetHash = false;
                g_value.spawnSetHash = 0;
            }
        }
        ImGui::EndCombo();
    }
    if (table) {
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("Spawn");
    ImGui::SetNextItemWidth(-1.0F);
    const char* spawnPreview = "Client picks";
    for (std::size_t i = 0; i < g_lists.spawnCount; ++i) {
        if (g_value.hasSpawnSetHash && g_value.spawnSetHash == g_lists.spawnHashes[i]) {
            spawnPreview = g_lists.spawns[i].data();
        }
    }
    if (ImGui::BeginCombo("##manual_spawn", spawnPreview)) {
        if (ImGui::Selectable("Client picks (no forced spawn set)", !g_value.hasSpawnSetHash)) {
            g_value.hasSpawnSetHash = false;
            g_value.spawnSetHash = 0;
        }
        auto lookup = g_value;
        lookup.hasSpawnSetHash = true;
        (void)options::validate_manual(lookup,
                                       g_validation); // One bounded catalog read per popup frame.
        for (std::size_t i = 0; i < g_lists.spawnCount; ++i) {
            auto candidate = g_value;
            candidate.hasSpawnSetHash = true;
            candidate.spawnSetHash = g_lists.spawnHashes[i];
            const bool supported =
                options::validate_manual(
                    candidate,
                    g_validation.layout,
                    std::span(g_validation.spawns).first(g_validation.spawnCount))
                == options::ManualError::none;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(g_lists.spawns[i].data(),
                                  g_value.hasSpawnSetHash
                                      && g_value.spawnSetHash == g_lists.spawnHashes[i])) {
                g_value.hasSpawnSetHash = true;
                g_value.spawnSetHash = g_lists.spawnHashes[i];
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if (g_lists.spawnUnavailable) {
        ImGui::TextWrapped(
            "Spawn metadata is unavailable. The client can still choose its own spawn.");
    }
}
} // namespace sunrise::client::ui::mission_launch::manual
