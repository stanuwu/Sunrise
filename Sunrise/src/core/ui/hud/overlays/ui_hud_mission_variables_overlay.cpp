/**
 * The mission variable overlay. It lists the newest variable changes any mission script
 * committed, newest at the bottom, so new rows push older ones up and off. It never fades: the
 * last changes stay on screen however long the script stays quiet.
 */

#include "ui_hud_mission_variables_overlay.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../../server/activity/mission/mission_script_variable_log.h"
#include "../../../settings/settings.h"
#include "../../runtime/settings.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::core::ui::hud::overlays::mission_variables {
namespace {

namespace variable_log = server::activity::mission::variable_log;
namespace mission_state = state::activity::mission;

// The setting can never ask for more rows than the log keeps.
static_assert(runtime::kMissionVariableRowsMaximum <= variable_log::kEntryCapacity);
/** How long a new row stays tinted, so a change is easy to spot as it lands. */
constexpr std::uint64_t kHighlightMs = 2000;
/** Strongest tint a new row starts with. */
constexpr float kHighlightAlpha = 0.35F;
/** Authored width of the age column. */
constexpr float kAgeColumnWidth = 34.0F;
/** Authored width of the key column. Long keys clip; the value gets the rest. */
constexpr float kKeyColumnWidth = 200.0F;
constexpr int kColumnCount = 3;
/** The Sunrise accent, as the theme defines it. */
constexpr ImVec4 kAccent{0.95F, 0.42F, 0.16F, 1.0F};
constexpr char kTitle[] = "Mission variables";

/** One formatted value. Strings are at most 127 bytes, plus quotes. */
using ValueText = std::array<char, 160>;
/** One formatted age. */
using AgeText = std::array<char, 16>;

variable_log::Snapshot g_snapshot{};

/** Writes one value the way a script would read it. */
void format_value(const mission_state::VariableValue& value, ValueText& output) noexcept {
    switch (value.kind) {
    case mission_state::VariableValueKind::boolean:
        (void)std::snprintf(
            output.data(), output.size(), "%s", value.booleanValue ? "true" : "false");
        return;
    case mission_state::VariableValueKind::integer:
        (void)std::snprintf(
            output.data(), output.size(), "%lld", static_cast<long long>(value.integerValue));
        return;
    case mission_state::VariableValueKind::real:
        (void)std::snprintf(output.data(), output.size(), "%.6g", value.realValue);
        return;
    case mission_state::VariableValueKind::string: {
        const std::size_t length =
            (std::min)(static_cast<std::size_t>(value.stringLength), value.stringValue.size());
        (void)std::snprintf(output.data(),
                            output.size(),
                            "\"%.*s\"",
                            static_cast<int>(length),
                            value.stringValue.data());
        return;
    }
    }
    output = {};
}

/** Writes how long ago a change landed, in the largest whole unit. */
void format_age(std::uint64_t now, std::uint64_t tick, AgeText& output) noexcept {
    const std::uint64_t seconds = now > tick ? (now - tick) / 1000U : 0U;
    if (seconds == 0) {
        (void)std::snprintf(output.data(), output.size(), "now");
    } else if (seconds < 60U) {
        (void)std::snprintf(
            output.data(), output.size(), "%llus", static_cast<unsigned long long>(seconds));
    } else if (seconds < 3600U) {
        (void)std::snprintf(
            output.data(), output.size(), "%llum", static_cast<unsigned long long>(seconds / 60U));
    } else {
        (void)std::snprintf(output.data(),
                            output.size(),
                            "%lluh",
                            static_cast<unsigned long long>(seconds / 3600U));
    }
}

/** @return The key's bytes as a bounded view. */
[[nodiscard]] std::string_view key_view(const mission_state::StateKey& key) noexcept {
    return {key.bytes.data(), (std::min)(static_cast<std::size_t>(key.length), key.bytes.size())};
}

/** @return True when the rows on screen came from more than one activity. */
[[nodiscard]] bool mixed_sources(std::size_t first) noexcept {
    for (std::size_t index = first + 1; index < g_snapshot.count; ++index) {
        if (std::string_view{g_snapshot.entries[index].activityId.data()}
            != std::string_view{g_snapshot.entries[first].activityId.data()}) {
            return true;
        }
    }
    return false;
}

/** Tints a row that landed recently, fading to nothing over the highlight window. */
void tint_recent(std::uint64_t now, std::uint64_t tick) noexcept {
    const std::uint64_t age = now > tick ? now - tick : 0U;
    if (age >= kHighlightMs) {
        return;
    }
    const float remaining = 1.0F - (static_cast<float>(age) / static_cast<float>(kHighlightMs));
    const ImVec4 tint{kAccent.x, kAccent.y, kAccent.z, kHighlightAlpha * remaining};
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
}

/** Draws one change as an age, a key and a value. */
void draw_row(const variable_log::Entry& entry, std::uint64_t now, bool showSource) noexcept {
    ImGui::TableNextRow();
    tint_recent(now, entry.tick);

    ImGui::TableNextColumn();
    AgeText age{};
    format_age(now, entry.tick, age);
    ImGui::TextDisabled("%s", age.data());

    ImGui::TableNextColumn();
    if (showSource) {
        ImGui::TextDisabled("%s", entry.activityId.data());
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::TextDisabled(":");
        ImGui::SameLine();
    }
    const std::string_view key = key_view(entry.key);
    ImGui::TextUnformatted(key.data(), key.data() + key.size());

    ImGui::TableNextColumn();
    if (entry.change == variable_log::Change::cleared) {
        ImGui::TextDisabled("cleared");
        return;
    }
    ValueText value{};
    format_value(entry.value, value);
    ImGui::TextUnformatted(value.data());
    if (entry.change == variable_log::Change::changed) {
        ValueText previous{};
        format_value(entry.previous, previous);
        ImGui::SameLine();
        ImGui::TextDisabled("was %s", previous.data());
    }
}

} // namespace

/** Draws the newest mission variable changes, newest last, inside the current HUD window. */
void draw() noexcept {
    // The ring only moves when a script commits a change, so most frames copy nothing.
    if (variable_log::newest_sequence() != g_snapshot.newestSequence) {
        variable_log::snapshot(g_snapshot);
    }
    ImGui::TextUnformatted(kTitle);
    ImGui::Separator();
    if (g_snapshot.count == 0) {
        ImGui::TextDisabled("no variable set yet");
        return;
    }
    // Rows on screen at once, from `client.ui.mission_variable_rows`. Older changes scroll off.
    const std::size_t visibleRows = (std::min)(
        static_cast<std::size_t>(settings::get().client.userInterface.missionVariableRows),
        variable_log::kEntryCapacity);
    const std::size_t first = g_snapshot.count - (std::min)(g_snapshot.count, visibleRows);
    const bool showSource = mixed_sources(first);
    const ImVec2 outerSize{ImGui::GetContentRegionAvail().x, 0.0F};
    if (!ImGui::BeginTable("##sunrise_hud_mission_variables",
                           kColumnCount,
                           ImGuiTableFlags_SizingFixedFit,
                           outerSize)) {
        return;
    }
    ImGui::TableSetupColumn(
        "age", ImGuiTableColumnFlags_WidthFixed, scaling::dpi::pixels(kAgeColumnWidth));
    ImGui::TableSetupColumn(
        "key", ImGuiTableColumnFlags_WidthFixed, scaling::dpi::pixels(kKeyColumnWidth));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    const std::uint64_t now = GetTickCount64();
    for (std::size_t index = first; index < g_snapshot.count; ++index) {
        draw_row(g_snapshot.entries[index], now, showSource);
    }
    ImGui::EndTable();
}

} // namespace sunrise::core::ui::hud::overlays::mission_variables
