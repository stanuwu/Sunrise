#include "sunrise_credits_badge.h"

#include <Windows.h>

#include <Shellapi.h>
#include <atomic>
#include <cstdint>
#include <imgui.h>

namespace sunrise::core::ui::layout::credits {
namespace {

/** ShellExecuteW reserves values through 32 for failure details. */
constexpr std::intptr_t kFirstShellSuccessValue = 33;
/** Zero height asks Dear ImGui to use the current themed frame height. */
constexpr float kAutomaticButtonHeight = 0.0F;

std::atomic_bool g_openPending{false};

/**
 * Opens one URL through the Windows shell after a visible user click.
 * @param url Null-terminated URL from the badge.
 * @return True when Windows accepts the open action.
 */
[[nodiscard]] bool open_with_shell(const wchar_t* url) noexcept {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(result) >= kFirstShellSuccessValue;
}

} // namespace

/** Runs one queued source action through a caller-supplied bridge. */
bool dispatch_pending(OpenUrlAction openUrl) noexcept {
    if (openUrl == nullptr || !g_openPending.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    return openUrl(kSourceUrl);
}

/** Queues one source action without leaving the active render call. */
void request_open() noexcept {
    g_openPending.store(true, std::memory_order_release);
}

/** Cancels a queued source action at the layout lifecycle boundary. */
void cancel_pending() noexcept {
    g_openPending.store(false, std::memory_order_release);
}

/** Runs one queued source action through the Windows shell bridge. */
void dispatch_pending() noexcept {
    (void)dispatch_pending(&open_with_shell);
}

/** Draws the full-width badge and queues the source URL only after a click. */
void draw() noexcept {
    const ImVec2 badgeSize{ImGui::GetContentRegionAvail().x, kAutomaticButtonHeight};
    if (ImGui::Button(kBadgeLabel, badgeSize)) {
        // Shell work waits until the synchronous present call has unwound.
        request_open();
    }
}

} // namespace sunrise::core::ui::layout::credits
