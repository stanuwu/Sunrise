#include "mission_launch_panel.h"

#include "../../../state/activity/forced/activity_forced_destination.h"
#include "../../../state/build_data/runtime.h"
#include "../../activity/mission_launch.h"
#include "mission_launch_art.h"
#include "mission_launch_cards.h"
#include "mission_launch_detail_style.h"
#include "mission_launch_manual.h"
#include "mission_launch_model.h"

namespace sunrise::client::ui::mission_launch {
namespace {
std::size_t g_content{};
unsigned g_libraryKind{};
int g_selected{-1};
bool g_custom{};
std::array<char, 96> g_search{};
std::array<char, 96> g_globalSearch{};
std::array<char, 64> g_type{};
bool g_showUnavailable{}, g_resetScroll{};
std::array<bool, state::build_data::activities::kCapacity> g_available{};
std::size_t g_layoutRevision{}, g_catalogRevision{};
std::array<std::uint16_t, state::build_data::activities::kCapacity> g_representatives{},
    g_variantCounts{};
void rebuild_experiences(std::span<const Activity> rows) noexcept {
    g_representatives.fill(0xFFFF);
    g_variantCounts.fill(0);
    for (const auto& row : rows) {
        if (!content_visible(content_group(row))) {
            continue;
        }
        const auto id = experience_id(row);
        ++g_variantCounts[id];
        auto& best = g_representatives[id];
        if (best == 0xFFFF
            || representative_score(row, g_available[row.index])
                   > representative_score(rows[best], g_available[best])) {
            best = row.index;
        }
    }
}
std::size_t visible_experiences(std::span<const Activity> rows,
                                std::span<std::uint16_t> output) noexcept {
    std::array<bool, state::build_data::activities::kCapacity> found{};
    std::size_t count{};
    for (const auto& row : rows) {
        if (!content_visible(content_group(row))) {
            continue;
        }
        const auto id = experience_id(row), representative = g_representatives[id];
        if (representative == 0xFFFF || found[id]
            || (!g_showUnavailable && !g_available[representative])) {
            continue;
        }
        if (!matches(row, g_content, 0, g_content == 0 ? g_globalSearch.data() : g_search.data())
            || (g_type[0] && experience_type(row) != g_type.data())) {
            continue;
        }
        found[id] = true;
        output[count++] = representative;
    }
    return count;
}
void navigate(std::size_t content, int selected = -1) noexcept {
    g_content = content;
    g_selected = selected;
    g_custom = false;
    g_resetScroll = true;
}
void reset_library() noexcept {
    g_globalSearch.fill(0);
    g_type.fill(0);
    navigate(0);
}
void breadcrumbs() noexcept {
    if (ImGui::Button("Content library")) {
        reset_library();
    }
    if (g_custom) {
        ImGui::TextDisabled("Custom Launch / Launch details");
        return;
    }
    if (g_content == 0) {
        if (g_globalSearch[0]) {
            ImGui::TextDisabled(g_selected >= 0 ? "Search results / Launch details"
                                                : "Search results across all content");
        }
        return;
    }
    if (ImGui::GetContentRegionAvail().x
        > ImGui::CalcTextSize(library_name(g_content)).x + 75.0F * card_scale()) {
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
    }
    if (ImGui::Button(library_name(g_content))) {
        navigate(g_content);
    }
    if (g_selected >= 0) {
        ImGui::TextDisabled("Launch details");
    }
}
void launch_controls(std::uint16_t index, bool manualMode) noexcept {
    namespace launch = client::activity::mission_launch;
    const auto rows = state::build_data::activities::entries();
    const auto status = launch::snapshot();
    state::activity::forced::ForcedDestination effective{};
    state::activity::forced::snapshot(effective);
    const bool overrideActive =
        state::activity::forced::active(effective) || state::activity::forced::override_active();
    const auto error = manualMode ? launch::validate_manual(manual::g_value, manual::g_validation)
                                  : launch::ManualError::none;
    const bool routeValid =
        index < rows.size() && g_available[index]
        && (!manualMode || launch::manual_transport_valid(index, manual::g_value, rows));
    const bool disabled = !routeValid || error != launch::ManualError::none || status.busy
                          || (!manualMode && overrideActive);
    if (manualMode) {
        ImGui::TextWrapped("Applies on Launch; the override remains active until cleared.");
    }
    if (detail::launch_button(status.busy, disabled, 43.0F * card_scale())) {
        if (manualMode) {
            (void)launch::request_manual(index, manual::g_value);
        } else {
            (void)launch::request(index);
        }
    }
    if (manualMode && error != launch::ManualError::none) {
        ImGui::TextWrapped("%s", launch::manual_error(error));
    } else if (!routeValid) {
        ImGui::TextWrapped(manualMode ? "Choose an available native launch activity."
                                      : "No direct-launch scenario is available for this variant.");
    } else if (status.busy || (status.index == index && status.status != launch::Status::idle)) {
        ImGui::TextWrapped("%s", launch::description(status.status));
    } else if (!overrideActive || manualMode) {
        ImGui::TextDisabled(manualMode ? "Launch from orbit."
                                       : "Launch from orbit. Native arrival.");
    }
    state::activity::forced::ForcedDestination stored{};
    state::activity::forced::snapshot(stored);
    if (state::activity::forced::active(stored)) {
        ImGui::TextWrapped("Override: %.*s",
                           static_cast<int>(stored.packageNameLength),
                           stored.packageName.data());
        ImGui::BeginDisabled(status.busy);
        if (ImGui::Button("Clear active override")) {
            state::activity::forced::clear();
        }
        ImGui::EndDisabled();
    }
}
void show_custom() noexcept {
    if (ImGui::Button("< Back to library")) {
        reset_library();
        return;
    }
    ImGui::Spacing();
    detail::heading("Custom Launch");
    ImGui::TextWrapped("Choose an activity, its launch route and arrival.");
    ImGui::Spacing();
    manual::custom_activity();
    manual::transport_picker(state::build_data::activities::entries(), g_available);
    ImGui::Spacing();
    manual::arrival_fields();
    ImGui::Spacing();
    launch_controls(manual::g_transport, true);
}
void show_detail(const Activity& row) noexcept {
    const auto rows = state::build_data::activities::entries();
    const auto id = experience_id(row);
    if (ImGui::Button("< Back to activities")) {
        navigate(g_content);
        return;
    }
    if (ImGui::GetContentRegionAvail().x > 150.0F * card_scale()) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Content library")) {
        reset_library();
        return;
    }
    ImGui::Spacing();
    const auto heading = title(row);
    const auto& display = state::build_data::activities::presentation(row.index);
    const auto icon = art::texture(activity_icon(row));
    if (ImGui::BeginTable(
            "##activity_heading", icon != 0 ? 2 : 1, ImGuiTableFlags_SizingStretchProp)) {
        if (icon != 0) {
            ImGui::TableSetupColumn(
                "##icon", ImGuiTableColumnFlags_WidthFixed, 66.0F * card_scale());
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            const auto origin = ImGui::GetCursorScreenPos();
            const float extent = 60.0F * card_scale();
            ImGui::Dummy({extent, extent});
            (void)draw_icon(activity_icon(row), origin, extent);
        }
        ImGui::TableNextColumn();
        detail::heading(heading.data());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(
            "%s  /  %s", experience_type(row).data(), library_name(content_group(row)));
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s",
        display.description[0]
            ? display.description.data()
            : "No description is available in this build's installed display metadata.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    manual::follow_activity(row);
    if (ImGui::Checkbox("Manual Mode", &manual::g_enabled) && manual::g_enabled) {
        manual::select_transport(row.index);
    }
    if (g_variantCounts[id] > 1) {
        std::array<char, 240> current{};
        const auto& p = state::build_data::activities::presentation(row.index);
        (void)std::snprintf(
            current.data(), current.size(), "Variant: %s (#%u)", p.type.data(), row.index);
        if (ImGui::GetContentRegionAvail().x >= 430.0F * card_scale()) {
            ImGui::SameLine(155.0F * card_scale());
        }
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo("##launch_variant", current.data())) {
            for (const auto& variant : rows) {
                if (experience_id(variant) != id || !content_visible(content_group(variant))) {
                    continue;
                }
                const auto& vp = state::build_data::activities::presentation(variant.index);
                std::array<char, 260> label{};
                (void)std::snprintf(label.data(),
                                    label.size(),
                                    "#%u | %s | %s%s",
                                    variant.index,
                                    vp.type.data(),
                                    title(variant).data(),
                                    g_available[variant.index] ? "" : " (unavailable)");
                if (ImGui::Selectable(label.data(), variant.index == row.index)) {
                    g_selected = variant.index;
                }
            }
            ImGui::EndCombo();
        }
        if (g_selected != row.index) {
            return;
        } // Never launch a stale selection in the same frame.
    }
    ImGui::Spacing();
    manual::arrival_fields();
    launch_controls(manual::g_enabled ? manual::g_transport : row.index, manual::g_enabled);
    ImGui::Spacing();
    if (ImGui::TreeNode("Activity information")) {
        ImGui::TextWrapped("First released: %s", kContent[content_group(row)]);
        ImGui::TextWrapped("%s", row.package[0] ? row.package.data() : "No direct destination");
        ImGui::Text("Activity #%u  |  %08X", row.index, row.hash);
        if (display.expansion[0]) {
            ImGui::TextWrapped("Installed variant label: %s", display.expansion.data());
        }
        ImGui::TextWrapped("Separate activity IDs preserve authored variants. An extracted "
                           "scenario does not guarantee complete mission scripting.");
        if (!display.title[0]) {
            ImGui::TextWrapped(
                "The native display name is unavailable; the exact package identifier is shown.");
        }
        ImGui::TreePop();
    }
}
} // namespace
void draw() noexcept {
    const detail::Style style{};
    namespace catalog = state::build_data::activities;
    const auto rows = catalog::entries();
    const auto revision = state::build_data::scenario_layout_count();
    if (revision != g_layoutRevision || rows.size() != g_catalogRevision) {
        g_available.fill(false);
        for (const auto& row : rows) {
            state::build_data::scenarios::Definition layout{};
            g_available[row.index] =
                !row.name().empty() && state::build_data::find_scenario_layout(row.name(), layout);
        }
        rebuild_experiences(rows);
        g_layoutRevision = revision;
        g_catalogRevision = rows.size();
    }
    if (!rows.empty()) {
        if (g_custom) {
            show_custom();
            return;
        }
        if (g_selected >= 0 && static_cast<std::size_t>(g_selected) < rows.size()) {
            show_detail(rows[static_cast<std::size_t>(g_selected)]);
            return;
        }
    }
    ImGui::TextWrapped("%s",
                       g_custom         ? "Choose a custom activity and manual arrival."
                       : g_content == 0 ? "Explore expansions, seasons and recurring events."
                       : g_selected < 0 ? "Choose an activity to review its details and launch."
                                        : "Review your activity and launch from orbit.");
    breadcrumbs();
    ImGui::Spacing();
    if (rows.empty()) {
        ImGui::TextWrapped(catalog::extraction_failed()
                               ? "The optional installed activity catalog could not be read. The "
                                 "Director remains available."
                               : "Reading installed Director metadata and artwork...");
        return;
    }
    const float scale = card_scale();
    if (g_content == 0) {
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##all_content_search",
                                     "Search activities across all content...",
                                     g_globalSearch.data(),
                                     g_globalSearch.size())) {
            g_libraryKind = 0;
            g_type.fill(0);
            g_resetScroll = true;
        }
        if (g_globalSearch[0] && ImGui::Button("Clear search")) {
            g_globalSearch.fill(0);
            g_type.fill(0);
            g_resetScroll = true;
        }
        if (!g_globalSearch[0]) {
            ImGui::TextWrapped("Activities are grouped by their first release. Equivalent launch "
                               "variants share one card; different activity types stay separate.");
            ImGui::Spacing();
            constexpr std::array<const char*, 4> kinds{
                "All content", "Expansions", "Seasons", "Events"};
            const auto kindColumns = static_cast<unsigned>(
                (std::max)(1,
                           static_cast<int>(ImGui::GetContentRegionAvail().x
                                            / (100.0F * scale + ImGui::GetStyle().ItemSpacing.x))));
            for (unsigned kind = 0; kind < kinds.size(); ++kind) {
                if (kind % kindColumns != 0) {
                    ImGui::SameLine();
                }
                if (ImGui::Selectable(kinds[kind], g_libraryKind == kind, 0, {100.0F * scale, 0})) {
                    g_libraryKind = kind;
                }
            }
            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0F * scale, 6.0F * scale});
            const int columns = grid_columns(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginTable("##dlc_grid", columns, ImGuiTableFlags_SizingStretchSame)) {
                if (g_libraryKind == 0) {
                    ImGui::TableNextColumn();
                    ImGui::PushID("custom_launch");
                    if (activity_card("Custom Launch",
                                      "Manual activity and arrival",
                                      "Choose Bubble / Slice / Spawn",
                                      state::build_data::activities::Icon{10},
                                      false)) {
                        manual::start_custom();
                        g_custom = true;
                    }
                    ImGui::PopID();
                }
                for (std::size_t content = 1; content < kContent.size(); ++content) {
                    if (!library_card_visible(content)
                        || (g_libraryKind != 0 && library_kind(content) != g_libraryKind)) {
                        continue;
                    }
                    ImGui::TableNextColumn();
                    std::size_t count{};
                    for (const auto index : g_representatives) {
                        if (index != 0xFFFF && library_group(rows[index]) == content
                            && g_available[index]) {
                            ++count;
                        }
                    }
                    std::array<char, 64> footer{};
                    (void)std::snprintf(
                        footer.data(), footer.size(), "%zu available activities", count);
                    ImGui::PushID(static_cast<int>(content));
                    if (activity_card(library_name(content),
                                      content == kUnresolvedContent ? "Release not established"
                                      : library_kind(content) == 3  ? "Recurring event"
                                      : library_kind(content) == 2  ? "Content season"
                                                                    : "Expansion / campaign",
                                      footer.data(),
                                      release_icon(content),
                                      false)) {
                        g_type.fill(0);
                        g_search.fill(0);
                        navigate(content);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            return;
        }
    }
    if (g_content != 0) {
        if (ImGui::Button("< Back to library")) {
            reset_library();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputTextWithHint("##mission_search",
                                     "Search name, package, ID or hash...",
                                     g_search.data(),
                                     g_search.size())) {
            g_resetScroll = true;
        }
    }
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::BeginCombo("##mission_type", g_type[0] ? g_type.data() : "All activity types")) {
        if (ImGui::Selectable("All activity types", !g_type[0])) {
            g_type.fill(0);
            g_resetScroll = true;
        }
        std::array<std::uint16_t, 256> unique{};
        std::size_t count{};
        for (const auto& row : rows) {
            if (!content_visible(content_group(row))
                || (g_content != 0 && library_group(row) != g_content)) {
                continue;
            }
            const auto type = experience_type(row);
            bool exists{};
            for (std::size_t i = 0; i < count; ++i) {
                exists |= experience_type(rows[unique[i]]) == type;
            }
            if (exists || count == unique.size()) {
                continue;
            }
            unique[count++] = row.index;
            if (ImGui::Selectable(type.data(), std::string_view(g_type.data()) == type)) {
                (void)std::snprintf(g_type.data(), g_type.size(), "%s", type.data());
                g_resetScroll = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Include unavailable scenarios", &g_showUnavailable)) {
        g_resetScroll = true;
    }
    std::array<std::uint16_t, catalog::kCapacity> visible{};
    const auto shown = visible_experiences(rows, visible);
    if (g_content == 20) {
        ImGui::TextWrapped("This event is present in the native metadata. Its public activity "
                           "catalog has no separate launch entry.");
    }
    ImGui::TextDisabled("%zu activities  |  Select a card for details", shown);
    const float height =
        (std::max)(150.0F * scale, ImGui::GetContentRegionAvail().y - 4.0F * scale);
    ImGui::BeginChild("mission_activity_grid", {0, height}, ImGuiChildFlags_None);
    if (g_resetScroll) {
        ImGui::SetScrollY(0);
        g_resetScroll = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0F * scale, 6.0F * scale});
    const int columns = grid_columns(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginTable("##activities", columns, ImGuiTableFlags_SizingStretchSame)) {
        ImGuiListClipper clipper;
        clipper.Begin((static_cast<int>(shown) + columns - 1) / columns);
        while (clipper.Step()) {
            for (int y = clipper.DisplayStart; y < clipper.DisplayEnd; ++y) {
                ImGui::TableNextRow();
                for (int x = 0; x < columns; ++x) {
                    const auto index = static_cast<std::size_t>(y * columns + x);
                    if (index >= shown) {
                        break;
                    }
                    ImGui::TableSetColumnIndex(x);
                    const auto& row = rows[visible[index]];
                    const auto heading = title(row);
                    std::array<char, 64> footer{};
                    (void)std::snprintf(footer.data(),
                                        footer.size(),
                                        "%u variant(s)  |  %s",
                                        static_cast<unsigned>(g_variantCounts[experience_id(row)]),
                                        g_available[row.index] ? "View launch details"
                                                               : "Scenario unavailable");
                    ImGui::PushID(row.index);
                    if (activity_card(heading.data(),
                                      experience_type(row).data(),
                                      footer.data(),
                                      activity_icon(row),
                                      false,
                                      g_available[row.index])) {
                        navigate(g_content, row.index);
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (shown == 0) {
        ImGui::TextWrapped(
            "No matching activities. Clear the search or choose another activity type. Unavailable "
            "playlist entries can be shown with the checkbox above.");
    }
    ImGui::EndChild();
}
} // namespace sunrise::client::ui::mission_launch
