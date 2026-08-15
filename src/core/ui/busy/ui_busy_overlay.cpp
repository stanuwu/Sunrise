#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>

#include "../scaling/dpi/ui_dpi_scaling.h"
#include "busy.h"
#include "ui_busy_state.h"

namespace sunrise::core::ui::busy {
namespace {

/** A hidden overlay draws nothing. */
constexpr float kHiddenAlpha = 0.0F;
/** A shown overlay draws opaque. It never fades in, so its first frame is already readable. */
constexpr float kShownAlpha = 1.0F;
/** The overlay leaves over this fade. Work resuming inside it is a dip, not a blink. */
constexpr float kFadeSeconds = 0.25F;
/** A 0.1 second cap stops a stalled frame from skipping the whole fade. */
constexpr float kMaximumDeltaSeconds = 0.1F;
/** Half the work area places the overlay on the horizontal center. */
constexpr float kHalfExtent = 2.0F;
/** 24 pixels keep the overlay clear of the viewport edge. */
constexpr float kViewportMargin = 24.0F;
/** 320 pixels fit the detail text on one line. */
constexpr float kProgressWidth = 320.0F;
/** Zero height asks Dear ImGui to use the current themed frame height. */
constexpr float kAutomaticProgressHeight = 0.0F;
/** A negative fraction picks Dear ImGui's indeterminate progress animation. */
constexpr float kIndeterminateRate = -1.0F;
/** The overlay is centered on the horizontal axis and pinned to the bottom edge. */
constexpr ImVec2 kBottomCenterPivot{0.5F, 1.0F};
/** The overlay carries no decoration, takes no input, and is never saved. */
constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
    | ImGuiWindowFlags_NoMove;
/** Zero height asks Dear ImGui to fit the window to its three fixed rows. */
constexpr float kAutomaticWindowHeight = 0.0F;
/** Window padding applies to both the left and the right edge. */
constexpr float kHorizontalPaddingCount = 2.0F;

/** One heading per task, in Task order. Each names the owner, so the stall is not read as
 * the game hanging. */
constexpr std::array<const char*, static_cast<std::size_t>(Task::count)> kHeadings{
    "Sunrise Indexing",
    "Sunrise Initializing",
    "Sunrise Extracting",
    "Sunrise Caching",
};
/** The detail line is the same for every task: what the user should do, not what runs. */
constexpr char kDetail[] = "Please be patient and do not close the game.";

/** Fade left after the last task ended. Owned by the presentation thread. */
float g_fadeSeconds{};
/** Last reported task, owned by the presentation thread. It survives the fade. */
std::size_t g_shownTask{};

/**
 * Finds the heading of the most recently started task.
 * Work nests, so the innermost task is the one that names what the stall is doing now.
 * @param running Mask of started tasks.
 * @return Index of the innermost started task, or the task count.
 */
[[nodiscard]] std::size_t innermost_task(unsigned running) noexcept {
    for (std::size_t index = kHeadings.size(); index != 0; --index) {
        if ((running & (1U << (index - 1))) != 0) {
            return index - 1;
        }
    }
    return kHeadings.size();
}

/**
 * Advances the fade and reports the alpha this frame draws with.
 * A task that ends starts the fade at once. Nothing is held open for work that may follow, so
 * a gap between two tasks closes the overlay and the next task opens its own.
 * @param running True while any task is started.
 * @param deltaSeconds Frame time, already capped.
 * @return Alpha between hidden and shown.
 */
[[nodiscard]] float advance_fade(bool running, float deltaSeconds) noexcept {
    if (running) {
        g_fadeSeconds = kFadeSeconds;
        return kShownAlpha;
    }
    g_fadeSeconds = g_fadeSeconds > deltaSeconds ? g_fadeSeconds - deltaSeconds : kHiddenAlpha;
    return g_fadeSeconds / kFadeSeconds;
}

} // namespace

/** Draws the running-work overlay inside the caller's active Dear ImGui frame. */
bool draw() noexcept {
    internal::set_present_thread(GetCurrentThreadId());
    const unsigned running = internal::running();
    const float delta = (std::min)(ImGui::GetIO().DeltaTime, kMaximumDeltaSeconds);
    const float alpha = advance_fade(running != 0, delta);
    if (alpha <= kHiddenAlpha) {
        return false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }
    // The last task stays on screen through the fade. Dropping the label at the start of it
    // would shrink the window by one row while it is still visible.
    const std::size_t task = innermost_task(running);
    if (task != kHeadings.size()) {
        g_shownTask = task;
    }

    const float margin = scaling::dpi::pixels(kViewportMargin);
    const float barWidth = scaling::dpi::pixels(kProgressWidth);
    const ImVec2 position{viewport->WorkPos.x + (viewport->WorkSize.x / kHalfExtent),
                          viewport->WorkPos.y + viewport->WorkSize.y - margin};
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, kBottomCenterPivot);
    // Fixed width and one label row keep the window one size, so it never jumps as work changes.
    ImGui::SetNextWindowSize(
        {barWidth + (ImGui::GetStyle().WindowPadding.x * kHorizontalPaddingCount),
         kAutomaticWindowHeight});
    // One style alpha fades the overlay and everything drawn inside it together.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    if (ImGui::Begin("##sunrise_busy", nullptr, kOverlayFlags)) {
        ImGui::TextUnformatted(kHeadings[g_shownTask]);
        ImGui::TextDisabled("%s", kDetail);
        ImGui::ProgressBar(kIndeterminateRate * static_cast<float>(ImGui::GetTime()),
                           {barWidth, kAutomaticProgressHeight},
                           "");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    // A faded frame does not prove the screen carries a readable overlay, which is what work
    // that stops the frame loop waits for.
    internal::record_drawn(alpha >= kShownAlpha);
    return true;
}

} // namespace sunrise::core::ui::busy
