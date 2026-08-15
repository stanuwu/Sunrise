#include "ui_notice_overlay.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>

#include "../animation/transition/ui_transition_animation.h"
#include "../scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::core::ui::notice {
namespace {

/** 4 notices cover a burst without hiding the first one that mattered. */
constexpr std::size_t kNoticeCapacity = 4;
/** 12 seconds is long enough to read a line and short enough to clear itself. */
constexpr float kNoticeSeconds = 12.0F;
/** A slot with no time left is free. */
constexpr float kExpired = 0.0F;
/** Fixed animation key. Every visibility-lane user needs its own, so keep these distinct. */
constexpr ImGuiID kOverlayAnimationId = 3;
/** Response rates for showing and hiding, matching the other overlays. */
constexpr animation::transition::Rates kVisibilityRates{16.0F, 14.0F};
/** A hidden overlay has finished its transition and draws nothing. */
constexpr float kHiddenProgress = 0.0F;
/** Half the work area places the overlay on the horizontal center. */
constexpr float kHalfExtent = 2.0F;
/** 24 pixels keep the overlay clear of the viewport edge. */
constexpr float kViewportMargin = 24.0F;
/** The overlay is centered on the horizontal axis and pinned to the top edge. */
constexpr ImVec2 kTopCenterPivot{0.5F, 0.0F};
/** The heading names the owner so the failure is not read as the game's. */
constexpr char kHeading[] = "Sunrise reported a problem";
/** The overlay carries no decoration, takes no input, and is never saved. */
constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
    | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

/** One message and the time it has left on screen. */
struct Entry {
    std::array<char, kTextCapacity> text{};
    std::size_t length{};
    float remainingSeconds{};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Entry, kNoticeCapacity> g_entries{};

/** @return A free slot, or else the one closest to expiry. */
[[nodiscard]] Entry& claim_slot() noexcept {
    Entry* oldest = &g_entries.front();
    for (Entry& entry : g_entries) {
        if (entry.remainingSeconds <= kExpired) {
            return entry;
        }
        if (entry.remainingSeconds < oldest->remainingSeconds) {
            oldest = &entry;
        }
    }
    return *oldest;
}

} // namespace

/**
 * Raises one user-visible notice about a Sunrise failure.
 * @param text Message. Longer text is cut.
 */
void raise(std::string_view text) noexcept {
    if (text.empty()) {
        return;
    }
    if (TryAcquireSRWLockExclusive(&g_lock) == FALSE) {
        // A fault handler must not wait behind the render thread, so this notice is dropped.
        return;
    }
    Entry& entry = claim_slot();
    entry.length = (std::min)(text.size(), kTextCapacity);
    std::copy_n(text.begin(), entry.length, entry.text.begin());
    entry.remainingSeconds = kNoticeSeconds;
    ReleaseSRWLockExclusive(&g_lock);
}

/**
 * Draws waiting notices inside the caller's active Dear ImGui frame.
 * @return True when draw data was built for them.
 */
bool draw() noexcept {
    std::array<Entry, kNoticeCapacity> pending{};
    std::size_t pendingCount = 0;
    const float deltaSeconds = ImGui::GetIO().DeltaTime;

    AcquireSRWLockExclusive(&g_lock);
    for (Entry& entry : g_entries) {
        if (entry.remainingSeconds <= kExpired) {
            continue;
        }
        entry.remainingSeconds = (std::max)(entry.remainingSeconds - deltaSeconds, kExpired);
        pending[pendingCount] = entry;
        ++pendingCount;
    }
    ReleaseSRWLockExclusive(&g_lock);

    const float progress = animation::transition::update(kOverlayAnimationId,
                                                         animation::transition::Lane::visibility,
                                                         pendingCount != 0,
                                                         kVisibilityRates,
                                                         kHiddenProgress);
    if (progress <= kHiddenProgress) {
        return false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }

    const float margin = scaling::dpi::pixels(kViewportMargin);
    const ImVec2 position{viewport->WorkPos.x + (viewport->WorkSize.x / kHalfExtent),
                          viewport->WorkPos.y + margin};
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, kTopCenterPivot);
    // One style alpha fades the overlay and everything drawn inside it together.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
    if (ImGui::Begin("##sunrise_notice", nullptr, kOverlayFlags)) {
        ImGui::TextUnformatted(kHeading);
        for (std::size_t index = 0; index < pendingCount; ++index) {
            const Entry& entry = pending[index];
            const char* first = entry.text.data();
            ImGui::TextUnformatted(first, first + entry.length);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return true;
}

} // namespace sunrise::core::ui::notice
