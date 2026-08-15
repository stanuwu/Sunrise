#include "ui_layout_lifecycle.h"

#include <Windows.h>

#include <algorithm>
#include <imgui.h>

#include "../animation/transition/ui_transition_animation.h"
#include "../fonts/runtime/ui_runtime_font_lifecycle.h"
#include "../memory/allocator.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "../theme/sunrise_ui_theme.h"
#include "credits/sunrise_credits_badge.h"

namespace sunrise::core::ui::layout {
namespace {

ImGuiContext* g_context{};
StateSnapshot g_state{};
SRWLOCK g_layoutLock{SRWLOCK_INIT};

} // namespace

/** Installs the fixed memory, runtime fonts, and the one Core-owned Dear ImGui context. */
bool initialize() noexcept {
    AcquireSRWLockExclusive(&g_layoutLock);
    if (g_state.initialized) {
        ReleaseSRWLockExclusive(&g_layoutLock);
        return true;
    }
    if (ImGui::GetCurrentContext() != nullptr || !memory::initialize()) {
        ReleaseSRWLockExclusive(&g_layoutLock);
        return false;
    }

    credits::cancel_pending();
    animation::transition::reset();
    g_context = ImGui::CreateContext();
    if (g_context == nullptr) {
        (void)memory::shutdown();
        ReleaseSRWLockExclusive(&g_layoutLock);
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Sunrise draws its own cursor while visible and never changes the game's OS cursor state.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    theme::apply();
    if (!fonts::runtime::initialize(nullptr)) {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        (void)memory::shutdown();
        ReleaseSRWLockExclusive(&g_layoutLock);
        return false;
    }
    g_state = {};
    g_state.initialized = true;
    ReleaseSRWLockExclusive(&g_layoutLock);
    return true;
}

/** Releases runtime fonts and destroys the context after both presentation backends stop. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_layoutLock);
    credits::cancel_pending();
    if (!fonts::runtime::shutdown()) {
        ReleaseSRWLockExclusive(&g_layoutLock);
        return false;
    }
    if (g_context != nullptr) {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
    }
    animation::transition::reset();
    scaling::dpi::reset();
    g_state = {};
    const bool released = memory::shutdown();
    ReleaseSRWLockExclusive(&g_layoutLock);
    return released;
}

/** @return One copy of the init and module selection state, read under the lock. */
StateSnapshot snapshot() noexcept {
    AcquireSRWLockShared(&g_layoutLock);
    const StateSnapshot result = g_state;
    ReleaseSRWLockShared(&g_layoutLock);
    return result;
}

namespace internal {

/** @return True when the calling thread has the Core-owned context current. */
bool context_is_current() noexcept {
    AcquireSRWLockShared(&g_layoutLock);
    const bool current = g_state.initialized && ImGui::GetCurrentContext() == g_context;
    ReleaseSRWLockShared(&g_layoutLock);
    return current;
}

/**
 * Replaces the selected module ID with one value from the registry snapshot.
 * @param stableId Registered ID that fits the fixed storage, or empty to clear the selection.
 */
void select_module(std::string_view stableId) noexcept {
    if (stableId.size() > modules::kStableIdCapacity) {
        return;
    }
    AcquireSRWLockExclusive(&g_layoutLock);
    if (g_state.initialized) {
        g_state.selectedStableId = {};
        std::copy(stableId.begin(), stableId.end(), g_state.selectedStableId.begin());
        g_state.selectedStableIdLength = stableId.size();
    }
    ReleaseSRWLockExclusive(&g_layoutLock);
}

} // namespace internal
} // namespace sunrise::core::ui::layout
