#include <algorithm>
#include <array>
#include <imgui.h>
#include <string_view>

#include "../../../../resources/resource.h"
#include "../animation/transition/ui_transition_animation.h"
#include "../components/card/ui_card_component.h"
#include "../components/logo/ui_logo_component.h"
#include "../components/section/ui_section_component.h"
#include "../modules/registry/ui_module_registry.h"
#include "../runtime/ui_visibility_runtime.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "navigation/ui_layout_navigation.h"
#include "ui_layout_lifecycle.h"

namespace sunrise::core::ui::layout {
namespace {

/** The authored width leaves room for a narrow menu and a wide settings panel. */
constexpr float kPreferredWindowWidth = 920.0F;
/** The authored height fits a 720p viewport with game space left around it. */
constexpr float kPreferredWindowHeight = 580.0F;
/** A 420-pixel minimum keeps the two columns from overlapping. */
constexpr float kMinimumWindowWidth = 420.0F;
/** A 300-pixel minimum keeps the navigation list and credits footer. */
constexpr float kMinimumWindowHeight = 300.0F;
/** 24 pixels keep the centered surface away from viewport edges. */
constexpr float kViewportMargin = 24.0F;
/** Two margins hold the same space on opposite viewport edges. */
constexpr float kViewportMarginCount = 2.0F;
/** 180 pixels caps the narrow module navigation. */
constexpr float kNavigationWidth = 180.0F;
/** Zero width lets Dear ImGui fill the space left on the current row. */
constexpr float kAutomaticWidth = 0.0F;
/** A half-axis pivot centers the window on both viewport axes. */
constexpr ImVec2 kCenterPivot{0.5F, 0.5F};
/** The main surface has no title bar and is left out of saved Dear ImGui state. */
constexpr ImGuiWindowFlags kMainWindowFlags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
    | ImGuiWindowFlags_NoTitleBar;
/** One trailing null byte turns a descriptor name into a component label. */
constexpr std::size_t kLabelTerminatorBytes = 1;
/** Fixed animation key. Every visibility-lane user needs its own, so keep these distinct. */
constexpr ImGuiID kSurfaceAnimationId = 1;
/** Response rates for opening and closing, in the same range as the other components. */
constexpr animation::transition::Rates kVisibilityRates{16.0F, 14.0F};
/** A closed surface has finished its transition and draws nothing. */
constexpr float kClosedProgress = 0.0F;
/** The surface grows from this fraction of its size while it opens. */
constexpr float kOpeningScale = 0.96F;
/** Full size, reached when the surface is fully open. */
constexpr float kOpenScale = 1.0F;
/** 34 authored pixels give the title logo presence without crowding the title row. */
constexpr float kTitleLogoExtent = 34.0F;
/** The title is drawn at this multiple of the body text, so it holds the logo's row. */
constexpr float kTitleTextRatio = 1.5F;
/** Half a difference centers one item against a taller one. */
constexpr float kHalfExtent = 2.0F;
/** The surface names the tool with the same wordmark the HUD card carries. */
constexpr char kTitle[] = "SUNRISE";
/** The hidden close action label is scoped by the main window. */
constexpr char kCloseWidgetId[] = "##close";
/** The close cross spans this fraction of its button, so it reads as a glyph and not a frame. */
constexpr float kCloseCrossRatio = 0.5F;
/** One pixel keeps the close cross as thin as the body text. */
constexpr float kCloseCrossThickness = 1.0F;

/**
 * Copies one display name into null-terminated component storage.
 * @return Fixed label storage, always with a trailing null.
 */
[[nodiscard]] std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes>
component_label(const modules::Descriptor& descriptor) noexcept {
    std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes> label{};
    const std::string_view displayName = descriptor.display_name();
    std::copy(displayName.begin(), displayName.end(), label.begin());
    return label;
}

/**
 * Works out a centered size that fits the viewport and the authored minimums.
 * @param viewport Active Dear ImGui viewport.
 * @return Main window size, or zero axes when the viewport is not ready.
 */
[[nodiscard]] ImVec2 window_size(const ImGuiViewport& viewport) noexcept {
    if (viewport.Size.x <= 0.0F || viewport.Size.y <= 0.0F) {
        return {};
    }
    const float margin = scaling::dpi::pixels(kViewportMargin);
    const float availableWidth = viewport.Size.x - (margin * kViewportMarginCount);
    const float availableHeight = viewport.Size.y - (margin * kViewportMarginCount);
    const float minimumWidth = scaling::dpi::pixels(kMinimumWindowWidth);
    const float minimumHeight = scaling::dpi::pixels(kMinimumWindowHeight);
    if (availableWidth < minimumWidth || availableHeight < minimumHeight) {
        return {};
    }
    return {(std::min)(scaling::dpi::pixels(kPreferredWindowWidth), availableWidth),
            (std::min)(scaling::dpi::pixels(kPreferredWindowHeight), availableHeight)};
}

/**
 * Draws the wide content panel and calls only the selected module's callback.
 * @param selected Descriptor copied from one registry snapshot.
 */
void draw_content(const navigation::Selection& selected) noexcept {
    if (!selected.moduleAvailable) {
        ImGui::TextDisabled("No modules are registered.");
        return;
    }
    const auto displayName = component_label(selected.descriptor);
    components::section::header(displayName.data());
    // One spacing height below the title row, so a module's first line never sits against it.
    ImGui::Dummy({kAutomaticWidth, ImGui::GetStyle().ItemSpacing.y});
    selected.descriptor.frame_callback()();
}

/** Draws optional module-owned companion windows after the main surface. */
void draw_companion_windows() noexcept {
    const modules::registry::RegistrySnapshot registrySnapshot = modules::registry::snapshot();
    for (const modules::Descriptor& descriptor : registrySnapshot.entries()) {
        const modules::FrameCallback callback = descriptor.companion_frame_callback();
        if (callback != nullptr) {
            callback();
        }
    }
}

/**
 * Draws the animated logo, then the name and version, on one title row, with a close action
 * at its far end.
 * @return True when the close action was pressed.
 */
[[nodiscard]] bool draw_title() noexcept {
    // The row edges are taken before it opens, so the close action can sit at its far end.
    const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
    const float rowRight = rowOrigin.x + ImGui::GetContentRegionAvail().x;
    const float extent = scaling::dpi::pixels(kTitleLogoExtent);
    const bool logoDrawn = components::logo::draw(extent);
    if (logoDrawn) {
        ImGui::SameLine();
    }

    // The size is the authored one, because the style carries the display scale separately.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kTitleTextRatio);
    const float titleHeight = ImGui::GetTextLineHeight();
    const float rowY = ImGui::GetCursorPosY();
    // The title is shorter than the logo, so it sits lower to stay level with it.
    const float titleY =
        logoDrawn ? rowY + ((std::max)(extent - titleHeight, 0.0F) / kHalfExtent) : rowY;
    ImGui::SetCursorPosY(titleY);
    ImGui::TextUnformatted(kTitle);
    ImGui::PopFont();

    ImGui::SameLine();
    // SameLine returns to the row the logo opened, so the version is placed against the title
    // again, centered on it because it stays at body size.
    ImGui::SetCursorPosY(
        titleY + ((std::max)(titleHeight - ImGui::GetTextLineHeight(), 0.0F) / kHalfExtent));
    ImGui::TextDisabled(SUNRISE_VER_STRING);

    // A cross at the right edge of the title row, centered on it. It stays on the row the logo
    // opened, like the version, so the row and the separator below keep their places.
    ImGui::SameLine();
    const float buttonExtent = ImGui::GetFontSize();
    const float rowHeight = logoDrawn ? extent : titleHeight;
    const ImVec2 buttonPosition{
        rowRight - buttonExtent,
        rowOrigin.y + ((std::max)(rowHeight - buttonExtent, 0.0F) / kHalfExtent)};
    ImGui::SetCursorScreenPos(buttonPosition);
    const bool pressed = ImGui::InvisibleButton(kCloseWidgetId, {buttonExtent, buttonExtent});

    // Muted like the version until hovered, then white like the title.
    const ImU32 color =
        ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float half = buttonExtent * kCloseCrossRatio / kHalfExtent;
    const ImVec2 center{buttonPosition.x + (buttonExtent / kHalfExtent),
                        buttonPosition.y + (buttonExtent / kHalfExtent)};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float thickness = scaling::dpi::pixels(kCloseCrossThickness);
    drawList->AddLine(
        {center.x - half, center.y - half}, {center.x + half, center.y + half}, color, thickness);
    drawList->AddLine(
        {center.x + half, center.y - half}, {center.x - half, center.y + half}, color, thickness);
    return pressed;
}

} // namespace

/** Draws the centered Sunrise surface inside the caller's active Dear ImGui frame. */
bool render(bool visible) noexcept {
    if (!internal::context_is_current()) {
        return false;
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }
    const ImVec2 size = window_size(*viewport);
    if (size.x <= 0.0F || size.y <= 0.0F) {
        return false;
    }
    // A new lane starts closed, so the surface animates open the first time it is asked for.
    const float progress = animation::transition::update(kSurfaceAnimationId,
                                                         animation::transition::Lane::visibility,
                                                         visible,
                                                         kVisibilityRates,
                                                         kClosedProgress);
    if (progress <= kClosedProgress) {
        return false;
    }

    const float scale = kOpeningScale + ((kOpenScale - kOpeningScale) * progress);
    const ImVec2 center = viewport->GetCenter();
    if (visible && progress < 1.0F) {
        // The opening zoom grows around the viewport centre. Only the settled window is movable.
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, kCenterPivot);
    } else {
        const ImVec2 centeredPosition{center.x - (size.x * kCenterPivot.x),
                                      center.y - (size.y * kCenterPivot.y)};
        ImGui::SetNextWindowPos(centeredPosition, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize({size.x * scale, size.y * scale}, ImGuiCond_Always);
    // One style alpha fades the surface and everything drawn inside it together.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
    const bool submitContents = ImGui::Begin("Sunrise", nullptr, kMainWindowFlags);
    if (submitContents) {
        // The close action only ever hides, so a surface already closing stays closed.
        if (draw_title()) {
            runtime::hide();
        }
        ImGui::Separator();

        const StateSnapshot state = snapshot();
        navigation::Selection selected{};
        const float panelHeight = ImGui::GetContentRegionAvail().y;
        {
            const components::card::Scope navigationCard(
                "##navigation_card", ImVec2(scaling::dpi::pixels(kNavigationWidth), panelHeight));
            if (navigationCard.visible()) {
                selected = navigation::draw(state);
            }
        }
        ImGui::SameLine();
        {
            const components::card::Scope contentCard("##content_card",
                                                      ImVec2(kAutomaticWidth, panelHeight));
            if (contentCard.visible()) {
                draw_content(selected);
            }
        }
    }
    ImGui::End();
    draw_companion_windows();
    ImGui::PopStyleVar();
    return true;
}

} // namespace sunrise::core::ui::layout
