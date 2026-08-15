#include <cstdint>
#include <imgui.h>
#include <string_view>

#include "../../../logging/snapshot/snapshot.h"
#include "../../components/toggle/ui_toggle_component.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "filters/logs_filter_controls.h"
#include "internal.h"

namespace sunrise::core::ui::modules::logs::internal {
namespace {

/** 500 authored pixels keep all three filters on one toolbar row. */
constexpr float kInlineFilterRowWidth = 500.0F;
/** 260 authored pixels fit both fixed-width selectors and their gap. */
constexpr float kInlineSelectorRowWidth = 260.0F;
/** 160 authored pixels fit the auto-scroll toggle. */
constexpr float kAutoScrollWidth = 160.0F;
/** 400 authored pixels fit both stats with a full-width replaced counter. */
constexpr float kInlineStatsRowWidth = 400.0F;
/** Fixed action label, also used to measure the row width. */
constexpr char kCopyVisibleLabel[] = "Copy Visible";
/** Shown until the window thread runs the copy. */
constexpr char kCopyPendingLabel[] = "Copy queued.";
/** Shown until the next request or a reset. */
constexpr char kCopyCompleteLabel[] = "Copied visible lines.";
/** Shown until the next request or a reset. */
constexpr char kCopyFailedLabel[] = "Clipboard copy failed.";
/** Zero size lets the log child take all the remaining space. */
constexpr ImVec2 kAutomaticChildSize{0.0F, 0.0F};
/** 1 scrolls to the bottom edge of the last line. */
constexpr float kScrollBottom = 1.0F;
/** One line per record; overflow scrolls sideways. */
constexpr ImGuiWindowFlags kLogWindowFlags =
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar;
/** A border separates the controls from the stored events. */
constexpr ImGuiChildFlags kLogChildFlags = ImGuiChildFlags_Borders;

/** User setting: follow the newest record instead of holding the current view. */
bool g_autoScroll{true};
/** Recorded count from the last drawn frame. A change in it means the log grew. */
std::uint64_t g_recordedCount{};

/**
 * Counts every stored event, including ones the ring replaced. The stored count alone stops
 * rising once the ring wraps.
 * @param visible Selection carrying its source snapshot counters.
 * @return Total that only rises, and only when a new record is stored.
 */
[[nodiscard]] std::uint64_t recorded_count(const log::view::Result& visible) noexcept {
    return visible.overwritten_count() + static_cast<std::uint64_t>(visible.source_count());
}

/** @param visible Filtered entries copied on click. */
void draw_copy_button(const log::view::Result& visible) noexcept {
    const float rowMaximumX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(kCopyVisibleLabel)) {
        (void)request_copy(visible);
    }
    const CopyStatus status = copy_status();
    const char* statusLabel = nullptr;
    if (status == CopyStatus::pending) {
        statusLabel = kCopyPendingLabel;
    } else if (status == CopyStatus::copied) {
        statusLabel = kCopyCompleteLabel;
    } else if (status == CopyStatus::failed) {
        statusLabel = kCopyFailedLabel;
    }
    if (statusLabel == nullptr) {
        return;
    }

    const float availableWidth = rowMaximumX - ImGui::GetItemRectMax().x;
    const ImGuiStyle& style = ImGui::GetStyle();
    if (copy_status_fits_inline(
            availableWidth, style.ItemSpacing.x, ImGui::CalcTextSize(statusLabel).x)) {
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s", statusLabel);
}

/**
 * Draws the stored records and moves the view to the newest one when asked.
 * @param visible Filtered entries, one line each.
 * @param follow True to put the newest line at the bottom of the view this frame.
 */
void draw_log_entries(const log::view::Result& visible, bool follow) noexcept {
    if (ImGui::BeginChild("##log_entries", kAutomaticChildSize, kLogChildFlags, kLogWindowFlags)) {
        if (visible.entries().empty()) {
            ImGui::TextDisabled("No retained events match these filters.");
        } else {
            for (const log::snapshot::Entry* entry : visible.entries()) {
                const std::string_view text = entry->text();
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
            }
            // Only on a frame that added a record. Pinning every frame drags the view back down
            // while the user reads older lines.
            if (follow) {
                ImGui::SetScrollHereY(kScrollBottom);
            }
        }
    }
    ImGui::EndChild();
}

} // namespace

/** Draws the Logs page inside the active Core UI frame. */
void draw() noexcept {
    ImGui::TextDisabled("Filter the bounded in-memory history or copy its visible lines.");
    ImGui::Separator();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool inlineSelectors = availableWidth >= scaling::dpi::pixels(kInlineSelectorRowWidth);
    const bool inlineFilters = availableWidth >= scaling::dpi::pixels(kInlineFilterRowWidth);
    filters::draw_channel();
    if (inlineSelectors) {
        ImGui::SameLine();
    }
    filters::draw_level();
    if (inlineFilters) {
        ImGui::SameLine();
    }
    filters::draw_text();

    const log::snapshot::Snapshot retained = log::snapshot::take();
    const log::view::Result visible = log::view::select(retained, filters::current());
    const std::uint64_t recorded = recorded_count(visible);
    const bool logGrew = recorded != g_recordedCount;
    g_recordedCount = recorded;
    const bool inlineStats =
        ImGui::GetContentRegionAvail().x >= scaling::dpi::pixels(kInlineStatsRowWidth);
    ImGui::Text("Visible: %zu / %zu", visible.entries().size(), visible.source_count());
    if (inlineStats) {
        ImGui::SameLine();
    }
    ImGui::TextDisabled("Replaced: %llu",
                        static_cast<unsigned long long>(visible.overwritten_count()));
    const float autoScrollWidth = scaling::dpi::pixels(kAutoScrollWidth);
    const float copyButtonWidth =
        ImGui::CalcTextSize(kCopyVisibleLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    const bool inlineActions =
        ImGui::GetContentRegionAvail().x
        >= autoScrollWidth + ImGui::GetStyle().ItemSpacing.x + copyButtonWidth;
    // Switching the toggle back on catches the view up without waiting for the next record.
    const bool followTurnedOn =
        components::toggle::control("Auto-scroll##log_auto_scroll", g_autoScroll, autoScrollWidth)
        && g_autoScroll;
    if (inlineActions) {
        ImGui::SameLine();
    }
    draw_copy_button(visible);
    ImGui::Separator();
    draw_log_entries(visible, g_autoScroll && (logGrew || followTurnedOn));
}

/** Clears local filter and copy-result state between UI lifecycles. */
void reset() noexcept {
    cancel_pending_copy();
    filters::reset();
    g_autoScroll = true;
    g_recordedCount = 0;
}

} // namespace sunrise::core::ui::modules::logs::internal
