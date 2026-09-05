#include <algorithm>
#include <array>
#include <imgui.h>
#include <string_view>

#include "../animation/transition/ui_transition_animation.h"
#include "../components/card/ui_card_component.h"
#include "../components/logo/ui_logo_component.h"
#include "../components/section/ui_section_component.h"
#include "../modules/registry/ui_module_registry.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "navigation/ui_layout_navigation.h"
#include "ui_layout_lifecycle.h"
#include "workspace_state.h"

namespace sunrise::core::ui::layout {
namespace {

workspace::State g_workspace{}, g_lastAttempt{};
bool g_loaded{};
float g_tabScroll{};
float g_lastScale{1};
float g_lastTabRegion{};
bool g_revealSelected{true};
bool g_brandDragged{};
modules::Descriptor g_active{};
bool g_activeRendered{};

constexpr float kStripHeight = 42;
constexpr float kControlWidth = 36;
constexpr float kBrandWidth = 132;
constexpr float kTabPadding = 12;
constexpr float kTabCloseWidth = 25;
constexpr float kTabTextGap = 4;
constexpr float kNavigationWidth = 180;
constexpr float kAutomaticWidth = 0;
constexpr auto kShell = IM_COL32(20, 22, 26, 255);
constexpr auto kStrip = IM_COL32(18, 20, 23, 255);
constexpr auto kAccent = IM_COL32(239, 125, 61, 255);

void deactivate() noexcept {
    if (g_activeRendered && g_active.deactivation_callback()) g_active.deactivation_callback()();
    g_activeRendered = false;
    g_active = {};
}

void persist() noexcept {
    if (g_loaded && g_workspace.positioned && g_workspace != g_lastAttempt) {
        (void)workspace::save(g_workspace);
        g_lastAttempt = g_workspace;
    }
}

const modules::Descriptor* find_tab(const modules::registry::RegistrySnapshot& registry,
                                    std::string_view id) noexcept {
    for (const auto& item : registry.entries())
        if (item.stable_id() == id && item.presentation() == modules::Presentation::workspaceTab)
            return &item;
    return nullptr;
}

void activate(std::string_view id) noexcept {
    if (workspace::open(g_workspace, id)) {
        g_revealSelected = true;
    }
}

enum class Icon { menu, close, left, right };

bool icon(const char* id,
          Icon shape,
          float width,
          float height,
          const char* tooltip,
          bool visible = true) noexcept {
    const auto start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {width, height});
    auto* draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddRectFilled(start,
                            {start.x + width, start.y + height},
                            ImGui::GetColorU32(IM_COL32(36, 39, 45, 255)),
                            scaling::dpi::pixels(3));
        ImGui::SetTooltip("%s", tooltip);
    }
    if (!visible && !hovered) return pressed;
    const ImVec2 center{start.x + width / 2, start.y + height / 2};
    const float unit = scaling::dpi::pixels(4.5F);
    const auto color = ImGui::GetColorU32(IM_COL32(160, 165, 175, 255));
    const auto line = [&](ImVec2 from, ImVec2 to) {
        draw->AddLine(from, to, color, scaling::dpi::pixels(1.3F));
    };
    if (shape == Icon::close) {
        line({center.x - unit, center.y - unit}, {center.x + unit, center.y + unit});
        line({center.x - unit, center.y + unit}, {center.x + unit, center.y - unit});
    } else if (shape == Icon::menu) {
        for (int n = -1; n <= 1; ++n)
            draw->AddCircleFilled(
                {center.x + n * unit, center.y}, scaling::dpi::pixels(1.2F), color);
    } else {
        const float direction = shape == Icon::left ? -1.0F : 1.0F;
        line({center.x - direction * unit / 2, center.y - unit},
             {center.x + direction * unit / 2, center.y});
        line({center.x + direction * unit / 2, center.y},
             {center.x - direction * unit / 2, center.y + unit});
    }
    return pressed;
}

void drag_strip(bool allowMaximize) noexcept {
    if (allowMaximize && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        g_workspace.maximized = !g_workspace.maximized;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        const auto delta = ImGui::GetIO().MouseDelta;
        const float dpi = scaling::dpi::current();
        if (!g_workspace.maximized) {
            g_workspace.restore.x += delta.x / dpi;
            g_workspace.restore.y += delta.y / dpi;
        }
    }
}

void brand(const ImVec2& origin, float width, float height, float dpi) noexcept {
    ImGui::SetCursorScreenPos({origin.x + 11 * dpi, origin.y + 9 * dpi});
    (void)components::logo::draw(24 * dpi);
    ImGui::SetCursorScreenPos(origin);
    const bool pressed = ImGui::InvisibleButton("##sunrise_home", {width, height});
    if (ImGui::IsItemActivated()) g_brandDragged = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) g_brandDragged = true;
    if (pressed && !g_brandDragged) {
        activate(workspace::kMainTabId);
    }
    drag_strip(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sunrise home - drag to move");

    auto* draw = ImGui::GetWindowDrawList();
    const float titleY = origin.y + (height - ImGui::GetTextLineHeight()) / 2;
    const bool home = std::string_view(g_workspace.selected.data()) == workspace::kMainTabId;
    draw->AddText(
        {origin.x + 43 * dpi, titleY},
        ImGui::GetColorU32(home ? IM_COL32(235, 236, 239, 255) : IM_COL32(176, 180, 188, 255)),
        "SUNRISE");
    draw->AddLine({origin.x + width, origin.y + 9 * dpi},
                  {origin.x + width, origin.y + height - 9 * dpi},
                  ImGui::GetColorU32(IM_COL32(55, 58, 65, 255)),
                  dpi);
}

void strip(const modules::registry::RegistrySnapshot& registry,
           float logicalWidth,
           float logicalHeight) noexcept {
    const float dpi = scaling::dpi::current();
    const float height = kStripHeight * dpi;
    const float control = kControlWidth * dpi;
    const float brandWidth = kBrandWidth * dpi;
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float remaining = (std::max)(1.0F, width - control - brandWidth);
    std::array<float, workspace::kTabCapacity> tabWidths{};
    float total{};
    for (std::size_t i = 1; i < g_workspace.tabCount; ++i) {
        const auto* module = find_tab(registry, g_workspace.tabs[i].data());
        const auto label = module ? module->display_name() : std::string_view{};
        tabWidths[i] = std::clamp(ImGui::CalcTextSize(label.data(), label.data() + label.size()).x
                                      + (kTabPadding + kTabTextGap + kTabCloseWidth) * dpi,
                                  76 * dpi,
                                  156 * dpi);
        total += tabWidths[i];
    }
    const bool overflow = total > remaining;
    const float arrows = overflow ? 40 * dpi : 0;
    const float tabRegion = (std::max)(1.0F, remaining - arrows);
    if (tabRegion != g_lastTabRegion) {
        g_revealSelected = true;
        g_lastTabRegion = tabRegion;
    }
    if (g_revealSelected) {
        float x{};
        for (std::size_t i = 1; i < g_workspace.tabCount; ++i) {
            if (g_workspace.tabs[i] == g_workspace.selected) {
                if (x < g_tabScroll) g_tabScroll = x;
                if (x + tabWidths[i] > g_tabScroll + tabRegion)
                    g_tabScroll = x + tabWidths[i] - tabRegion;
            }
            x += tabWidths[i];
        }
        g_revealSelected = false;
    }
    g_tabScroll = std::clamp(g_tabScroll, 0.0F, (std::max)(0.0F, total - tabRegion));

    brand(origin, brandWidth, height, dpi);
    ImGui::SetCursorScreenPos({origin.x + brandWidth, origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{});
    ImGui::BeginChild("##tabs",
                      {tabRegion, height},
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                          | ImGuiWindowFlags_NoBackground);
    if (ImGui::IsWindowHovered())
        g_tabScroll = std::clamp(g_tabScroll - ImGui::GetIO().MouseWheel * 80 * dpi,
                                 0.0F,
                                 (std::max)(0.0F, total - tabRegion));
    const auto tabsOrigin = ImGui::GetCursorScreenPos();
    float x = -g_tabScroll;
    std::size_t closeIndex = workspace::kTabCapacity;
    workspace::TabId moving{};
    std::size_t moveTarget = workspace::kTabCapacity;
    for (std::size_t i = 1; i < g_workspace.tabCount; ++i) {
        const auto* module = find_tab(registry, g_workspace.tabs[i].data());
        if (!module) continue;
        const auto label = module->display_name();
        const bool active = g_workspace.tabs[i] == g_workspace.selected;
        const ImVec2 position{tabsOrigin.x + x, tabsOrigin.y};
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID(g_workspace.tabs[i].data());
        if (ImGui::InvisibleButton("##tab", {tabWidths[i] - kTabCloseWidth * dpi, height}))
            activate(module->stable_id());
        const bool tabHovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        if (active || tabHovered) {
            draw->AddRectFilled(
                position,
                {position.x + tabWidths[i], position.y + height},
                ImGui::GetColorU32(active ? IM_COL32(25, 28, 32, 255) : IM_COL32(31, 34, 39, 255)));
        }
        if (active) {
            draw->AddRectFilled({position.x + 12 * dpi, position.y + height - 2 * dpi},
                                {position.x + tabWidths[i] - 12 * dpi, position.y + height},
                                ImGui::GetColorU32(kAccent));
        }
        const float textY = position.y + (height - ImGui::GetTextLineHeight()) / 2;
        // Truncate long tool names at character boundaries, with a visible ellipsis.
        const float textRight = position.x + tabWidths[i] - (kTabCloseWidth + kTabTextGap) * dpi;
        const float textLeft = position.x + kTabPadding * dpi;
        const float ellipsisWidth = ImGui::CalcTextSize("...").x;
        auto shown = label;
        const bool truncated = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x
                               > textRight - textLeft + 0.5F;
        while (truncated && !shown.empty()
               && ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x + ellipsisWidth
                      > textRight - textLeft) {
            auto end = shown.size() - 1;
            while (end > 0 && (static_cast<unsigned char>(shown[end]) & 0xC0) == 0x80)
                --end;
            shown = shown.substr(0, end);
        }
        draw->PushClipRect(position, {textRight, position.y + height}, true);
        const auto textColor = ImGui::GetColorU32(active ? IM_COL32(235, 232, 230, 255)
                                                         : IM_COL32(157, 162, 172, 255));
        draw->AddText({textLeft, textY}, textColor, shown.data(), shown.data() + shown.size());
        if (truncated)
            draw->AddText(
                {textLeft + ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x,
                 textY},
                textColor,
                "...");
        draw->PopClipRect();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.*s", static_cast<int>(label.size()), label.data());
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(
                "SUNRISE_TAB", g_workspace.tabs[i].data(), sizeof(workspace::TabId));
            ImGui::TextUnformatted(label.data(), label.data() + label.size());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("SUNRISE_TAB")) {
                if (payload->DataSize == sizeof(workspace::TabId)) {
                    std::copy_n(
                        static_cast<const char*>(payload->Data), moving.size(), moving.begin());
                    moveTarget = i;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetCursorScreenPos({position.x + tabWidths[i] - kTabCloseWidth * dpi, position.y});
        if (icon("##close_tab",
                 Icon::close,
                 kTabCloseWidth * dpi,
                 height,
                 "Close tool",
                 active || tabHovered))
            closeIndex = i;
        ImGui::PopID();
        x += tabWidths[i];
    }
    if (total < tabRegion) {
        ImGui::SetCursorScreenPos({tabsOrigin.x + total, tabsOrigin.y});
        ImGui::InvisibleButton("##drag_strip", {(std::max)(1.0F, tabRegion - total), height});
        drag_strip(true);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    if (closeIndex != workspace::kTabCapacity) {
        workspace::close(g_workspace, closeIndex);
        g_revealSelected = true;
    }
    if (moveTarget != workspace::kTabCapacity) {
        for (std::size_t i = 1; i < g_workspace.tabCount; ++i) {
            if (g_workspace.tabs[i] == moving) {
                workspace::move(g_workspace, i, moveTarget);
                break;
            }
        }
    }

    ImGui::SetCursorScreenPos({origin.x + brandWidth + tabRegion, origin.y});
    if (overflow) {
        if (icon("##scroll_left", Icon::left, 20 * dpi, height, "Scroll tools left"))
            g_tabScroll = (std::max)(0.0F, g_tabScroll - 140 * dpi);
        ImGui::SetCursorScreenPos({origin.x + brandWidth + tabRegion + 20 * dpi, origin.y});
        if (icon("##scroll_right", Icon::right, 20 * dpi, height, "Scroll tools right"))
            g_tabScroll = (std::min)((std::max)(0.0F, total - tabRegion), g_tabScroll + 140 * dpi);
    }

    const float controlsX = origin.x + width - control;
    ImGui::SetCursorScreenPos({controlsX, origin.y});
    if (icon("##workspace_menu", Icon::menu, control, height, "Workspace options"))
        ImGui::OpenPopup("##workspace_options");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12 * dpi, 10 * dpi});
    if (ImGui::BeginPopup("##workspace_options")) {
        ImGui::TextDisabled("WORKSPACE");
        if (ImGui::Selectable(
                "Sunrise", std::string_view(g_workspace.selected.data()) == workspace::kMainTabId))
            activate(workspace::kMainTabId);
        for (std::size_t i = 1; i < g_workspace.tabCount; ++i) {
            if (const auto* module = find_tab(registry, g_workspace.tabs[i].data())) {
                std::array<char, modules::kDisplayNameCapacity + 1> name{};
                std::copy(
                    module->display_name().begin(), module->display_name().end(), name.begin());
                if (ImGui::Selectable(name.data(), g_workspace.tabs[i] == g_workspace.selected))
                    activate(module->stable_id());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            workspace::reset(g_workspace, logicalWidth, logicalHeight);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos({origin.x, origin.y + height});
}

[[nodiscard]] std::array<char, modules::kDisplayNameCapacity + 1>
component_label(const modules::Descriptor& descriptor) noexcept {
    std::array<char, modules::kDisplayNameCapacity + 1> label{};
    std::copy(descriptor.display_name().begin(), descriptor.display_name().end(), label.begin());
    return label;
}

void draw_active(const modules::Descriptor& descriptor) noexcept {
    if (g_activeRendered
        && (g_active.stable_id() != descriptor.stable_id()
            || g_active.frame_callback() != descriptor.frame_callback()))
        deactivate();
    g_active = descriptor;
    g_activeRendered = true;
    descriptor.frame_callback()();
}

bool draw_main_page() noexcept {
    navigation::Selection selected{};
    const float panelHeight = ImGui::GetContentRegionAvail().y;
    {
        const components::card::Scope navigationCard(
            "##navigation_card", ImVec2(scaling::dpi::pixels(kNavigationWidth), panelHeight));
        if (navigationCard.visible()) selected = navigation::draw(snapshot());
    }
    ImGui::SameLine();
    bool rendered{};
    {
        const components::card::Scope contentCard("##content_card",
                                                  ImVec2(kAutomaticWidth, panelHeight));
        if (contentCard.visible()) {
            if (!selected.moduleAvailable) {
                deactivate();
                ImGui::TextDisabled("No main-menu modules are registered.");
            } else {
                const auto label = component_label(selected.descriptor);
                components::section::header(label.data());
                ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y});
                draw_active(selected.descriptor);
                rendered = true;
            }
        }
    }
    return rendered;
}

} // namespace

bool render(bool visible) noexcept {
    if (!internal::context_is_current()) return false;
    const auto* viewport = ImGui::GetMainViewport();
    const float dpi = scaling::dpi::current();
    if (!viewport || dpi <= 0 || viewport->WorkSize.x <= 0 || viewport->WorkSize.y <= 0) {
        deactivate();
        return false;
    }
    if (dpi != g_lastScale) {
        g_tabScroll *= dpi / g_lastScale;
        g_lastScale = dpi;
        g_revealSelected = true;
    }
    if (!g_loaded) {
        workspace::load(g_workspace);
        g_lastAttempt = g_workspace;
        g_loaded = true;
    }
    const auto registry = modules::registry::snapshot();
    std::array<std::string_view, modules::registry::kModuleCapacity + 1> ids{};
    std::size_t count{};
    ids[count++] = workspace::kMainTabId;
    for (const auto& module : registry.entries()) {
        if (module.presentation() == modules::Presentation::workspaceTab)
            ids[count++] = module.stable_id();
    }
    workspace::reconcile(g_workspace, {ids.data(), count});

    // Hiding the menu leaves World helpers active, as upstream does. Only losing the owning
    // tool (including removal while hidden) relinquishes its presentation state here.
    if (g_activeRendered && g_active.presentation() == modules::Presentation::workspaceTab) {
        const auto* selected = find_tab(registry, g_workspace.selected.data());
        if (!selected || g_active.stable_id() != selected->stable_id()
            || g_active.frame_callback() != selected->frame_callback())
            deactivate();
    }

    const float progress = animation::transition::update(
        1, animation::transition::Lane::visibility, visible, {16, 14}, 0);
    if (progress <= 0) {
        persist();
        return false;
    }
    const float logicalWidth = viewport->WorkSize.x / dpi;
    const float logicalHeight = viewport->WorkSize.y / dpi;
    const auto rectangle = workspace::display(g_workspace, logicalWidth, logicalHeight);
    ImGui::SetNextWindowPos(
        {viewport->WorkPos.x + rectangle.x * dpi, viewport->WorkPos.y + rectangle.y * dpi},
        ImGuiCond_Always);
    ImGui::SetNextWindowSize({rectangle.width * dpi, rectangle.height * dpi}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        {(std::min)(420.0F, logicalWidth) * dpi, (std::min)(300.0F, logicalHeight) * dpi},
        viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7 * dpi);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kShell));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{.19F, .20F, .23F, 1});
    const auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                       | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove
                       | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                       | (g_workspace.maximized ? ImGuiWindowFlags_NoResize : 0)
                       | (!visible ? ImGuiWindowFlags_NoInputs : 0);
    bool pageRendered{};
    if (ImGui::Begin("Sunrise", nullptr, flags)) {
        if (!g_workspace.maximized) {
            const auto position = ImGui::GetWindowPos();
            const auto size = ImGui::GetWindowSize();
            g_workspace.restore = {(position.x - viewport->WorkPos.x) / dpi,
                                   (position.y - viewport->WorkPos.y) / dpi,
                                   size.x / dpi,
                                   size.y / dpi};
        }
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            origin,
            {origin.x + ImGui::GetWindowSize().x, origin.y + kStripHeight * dpi},
            ImGui::GetColorU32(kStrip),
            7 * dpi,
            ImDrawFlags_RoundCornersTop);
        ImGui::GetWindowDrawList()->AddLine(
            {origin.x, origin.y + kStripHeight * dpi},
            {origin.x + ImGui::GetWindowSize().x, origin.y + kStripHeight * dpi},
            ImGui::GetColorU32(IM_COL32(50, 53, 60, 255)),
            dpi);
        strip(registry, logicalWidth, logicalHeight);

        const bool mainSelected =
            std::string_view(g_workspace.selected.data()) == workspace::kMainTabId;
        const auto* selectedTab =
            mainSelected ? nullptr : find_tab(registry, g_workspace.selected.data());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12 * dpi, 16 * dpi});
        ImGui::PushID(g_workspace.selected.data());
        if (ImGui::BeginChild("##workspace_content",
                              {0, 0},
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoSavedSettings)) {
            if (mainSelected && visible) {
                pageRendered = draw_main_page();
            } else if (selectedTab && visible) {
                draw_active(*selectedTab);
                pageRendered = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar();
    }
    ImGui::End();
    if (visible && !pageRendered) deactivate();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!ImGui::IsMouseDown(0) || !visible) persist();
    return true;
}

void open_workspace_tab(std::string_view stableId) noexcept {
    const auto registry = modules::registry::snapshot();
    if (find_tab(registry, stableId) != nullptr) activate(stableId);
}

bool workspace_tab_open(std::string_view stableId) noexcept {
    for (std::size_t index = 1; index < g_workspace.tabCount; ++index)
        if (std::string_view(g_workspace.tabs[index].data()) == stableId) return true;
    return false;
}

void set_workspace_tab_open(std::string_view stableId, bool open) noexcept {
    if (open) {
        open_workspace_tab(stableId);
        return;
    }
    for (std::size_t index = 1; index < g_workspace.tabCount; ++index) {
        if (std::string_view(g_workspace.tabs[index].data()) == stableId) {
            workspace::close(g_workspace, index);
            g_revealSelected = true;
            return;
        }
    }
}

namespace internal {
void reset_workspace() noexcept {
    deactivate();
    persist();
    g_workspace = {};
    g_lastAttempt = {};
    g_loaded = false;
    g_tabScroll = 0;
    g_lastScale = 1;
    g_lastTabRegion = 0;
    g_revealSelected = true;
    g_brandDragged = false;
}
} // namespace internal
} // namespace sunrise::core::ui::layout
