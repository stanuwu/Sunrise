#include "ui_layout_navigation.h"

#include <algorithm>
#include <array>
#include <imgui.h>
#include <string_view>

#include "../../components/navigation/ui_navigation_component.h"
#include "../../modules/registry/ui_module_registry.h"
#include "../credits/sunrise_credits_badge.h"
#include "../ui_layout_lifecycle.h"

namespace sunrise::core::ui::layout::navigation {
namespace {

/** One trailing null byte turns a descriptor label into a Dear ImGui label. */
constexpr std::size_t kLabelTerminatorBytes = 1;
/** One pixel keeps the module child valid when the viewport is unusually short. */
constexpr float kMinimumModuleListHeight = 1.0F;
/** Zero width gives the module child all available navigation width. */
constexpr float kAutomaticChildWidth = 0.0F;
/** The module list never writes a Dear ImGui settings record. */
constexpr ImGuiWindowFlags kModuleListWindowFlags = ImGuiWindowFlags_NoSavedSettings;

using modules::Descriptor;
using modules::registry::RegistrySnapshot;

/**
 * Copies one display name into null-terminated widget storage.
 * @return Fixed label storage, always with a trailing null.
 */
[[nodiscard]] std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes>
widget_label(const Descriptor& descriptor) noexcept {
    std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes> label{};
    const std::string_view displayName = descriptor.display_name();
    std::copy(displayName.begin(), displayName.end(), label.begin());
    return label;
}

/**
 * Finds the saved selection in one registry snapshot.
 * @param registrySnapshot Copied modules in menu order.
 * @param state Selection state taken before the registry was read.
 * @return Copied descriptor, or an unavailable value for an empty registry.
 */
[[nodiscard]] Selection resolve_selection(const RegistrySnapshot& registrySnapshot,
                                          const StateSnapshot& state) noexcept {
    const auto entries = registrySnapshot.entries();
    if (entries.empty()) {
        internal::select_module({});
        return {};
    }

    const std::string_view selectedId(state.selectedStableId.data(), state.selectedStableIdLength);
    for (const Descriptor& descriptor : entries) {
        if (descriptor.stable_id() == selectedId) {
            return {descriptor, true};
        }
    }
    // Registry removal falls back to the first module in menu order.
    internal::select_module(entries.front().stable_id());
    return {entries.front(), true};
}

/**
 * Draws the module list and applies a clicked selection by stable ID.
 * @param registrySnapshot Copied modules in menu order.
 * @param selected Selection found before the list is drawn.
 * @return The descriptor selected after list input.
 */
[[nodiscard]] Selection draw_module_choices(const RegistrySnapshot& registrySnapshot,
                                            Selection selected) noexcept {
    for (const Descriptor& descriptor : registrySnapshot.entries()) {
        const std::string_view stableId = descriptor.stable_id();
        ImGui::PushID(stableId.data(), stableId.data() + stableId.size());
        const auto label = widget_label(descriptor);
        if (components::navigation::tab(label.data(),
                                        selected.moduleAvailable
                                            && selected.descriptor.stable_id()
                                                   == descriptor.stable_id())) {
            internal::select_module(descriptor.stable_id());
            selected = {descriptor, true};
        }
        ImGui::PopID();
    }
    return selected;
}

} // namespace

/** Draws the narrow module menu without retaining registry storage. */
Selection draw(const StateSnapshot& state) noexcept {
    ImGui::TextUnformatted("MODULES");
    ImGui::Separator();
    const RegistrySnapshot registrySnapshot = modules::registry::snapshot();
    Selection selected = resolve_selection(registrySnapshot, state);
    const float footerHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    const float listHeight =
        (std::max)(ImGui::GetContentRegionAvail().y - footerHeight, kMinimumModuleListHeight);
    if (ImGui::BeginChild("##module_list",
                          ImVec2(kAutomaticChildWidth, listHeight),
                          ImGuiChildFlags_None,
                          kModuleListWindowFlags)) {
        if (registrySnapshot.entries().empty()) {
            ImGui::TextDisabled("No modules registered.");
        } else {
            selected = draw_module_choices(registrySnapshot, selected);
        }
    }
    ImGui::EndChild();
    credits::draw();
    return selected;
}

} // namespace sunrise::core::ui::layout::navigation
