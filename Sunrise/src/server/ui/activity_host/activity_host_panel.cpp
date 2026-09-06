#include "activity_host_panel.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>

#include "../../../client/hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../activity/host_runtime.h"
#include "../../bap/runtime.h"
#include "activity_host_event_view.h"
#include "activity_host_sdk_view.h"

namespace sunrise::server::ui::activity_host {
namespace {

namespace host = server::activity::host;
namespace section = core::ui::components::section;

host::DiagnosticsSnapshot g_snapshot{};
state::activity::SessionBinding g_selected{};
bool g_followPlayer{true};
bool g_showSdkWindow{};
bool g_showEventsWindow{};

/** @return True when compact ingress identity names the selected activity generation. */
[[nodiscard]] bool same_binding(const host::ClientMessageBinding& left,
                                const state::activity::SessionBinding& right) noexcept {
    return left.sessionId == right.sessionId && left.createdRevision == right.createdRevision;
}

/** Writes one short activity label for a selector or table. */
void instance_label(const state::activity::SessionBinding& binding,
                    bool active,
                    std::size_t linkCount,
                    std::span<char> output) noexcept {
    const auto& destination = binding.destination;
    const std::string_view name(reinterpret_cast<const char*>(destination.packageName.data()),
                                destination.packageNameLength);
    (void)std::snprintf(output.data(),
                        output.size(),
                        "%.*s (%s, %s) [%llX.%llu]",
                        static_cast<int>(name.size()),
                        name.data(),
                        active ? "active" : "inactive",
                        linkCount != 0 ? "linked" : "unlinked",
                        static_cast<unsigned long long>(binding.sessionId),
                        static_cast<unsigned long long>(binding.createdRevision));
}

/** Finds the currently selected instance in the copied snapshot. */
[[nodiscard]] const host::InstanceSnapshot* selected_instance() noexcept {
    for (std::size_t index = 0; index < g_snapshot.instanceCount; ++index) {
        if (same_binding(g_snapshot.instances[index].binding, g_selected)) {
            return &g_snapshot.instances[index];
        }
    }
    return nullptr;
}

/** Follows the player's region host, or retains a valid manual selection when following is off. */
void select_default() noexcept {
    if (g_followPlayer) {
        g_selected = {};
        const auto local = client::hooks::bootflow::current_slice_set();
        server::bap::CurrentActivityLinkView current{};
        if (!local.present
            || !server::bap::current_activity_host_link_view(local.index, current)
            || current.effectiveRegion != local.index) {
            return;
        }
        server::bap::ActivityLinkView exact{};
        if (!server::bap::activity_link_view(current.binding, exact)
            || exact.matchingLinks != 1
            || exact.activityClientGeneration != current.activityClientGeneration
            || exact.effectiveRegion != local.index) {
            return;
        }
        for (std::size_t index = 0; index < g_snapshot.instanceCount; ++index) {
            const host::InstanceSnapshot& instance = g_snapshot.instances[index];
            if (instance.active && same_binding(instance.binding, current.binding)) {
                g_selected = instance.binding;
                return;
            }
        }
        return;
    }
    if (g_snapshot.instanceCount == 0) {
        g_selected = {};
        return;
    }
    if (selected_instance() != nullptr) {
        return;
    }
    std::size_t selected = 0;
    for (std::size_t index = 0; index < g_snapshot.instanceCount; ++index) {
        const host::InstanceSnapshot& instance = g_snapshot.instances[index];
        if (instance.active && server::bap::activity_link_count(instance.binding) != 0) {
            selected = index;
            break;
        }
        if (instance.active && !g_snapshot.instances[selected].active) {
            selected = index;
        }
    }
    g_selected = g_snapshot.instances[selected].binding;
}

/** Draws the exact Activity Host instance selector. */
void draw_instance_selector() noexcept {
    if (ImGui::Checkbox("Follow player", &g_followPlayer)) {
        select_default();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically targets the player's current Activity Host. "
                         "Turn off to inspect another instance manually.");
    }
    ImGui::BeginDisabled(g_followPlayer);
    const host::InstanceSnapshot* current = selected_instance();
    std::array<char, 112> preview{};
    if (current != nullptr) {
        instance_label(current->binding,
                       current->active,
                       server::bap::activity_link_count(current->binding),
                       preview);
    } else {
        (void)std::snprintf(preview.data(), preview.size(), "no activity");
    }
    if (!ImGui::BeginCombo("Instance", preview.data())) {
        ImGui::EndDisabled();
        if (g_followPlayer && current == nullptr) {
            ImGui::TextDisabled("Waiting for the player's current instance");
        }
        return;
    }
    for (std::size_t index = 0; index < g_snapshot.instanceCount; ++index) {
        const host::InstanceSnapshot& instance = g_snapshot.instances[index];
        std::array<char, 112> label{};
        instance_label(instance.binding,
                       instance.active,
                       server::bap::activity_link_count(instance.binding),
                       label);
        const bool selected = same_binding(instance.binding, g_selected);
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(label.data(), selected)) {
            g_selected = instance.binding;
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    ImGui::EndDisabled();
}

/** Draws generated SDK data in its own movable window. */
void draw_sdk_window(const host::InstanceSnapshot* instance) noexcept {
    sdk_view::draw(g_showSdkWindow, instance);
}

/** Draws the compact window launcher. */
void draw_launcher(const host::InstanceSnapshot* instance) noexcept {
    section::header("Windows", nullptr);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each tool opens as its own movable window.");
    }
    ImGui::Checkbox("World", &g_showSdkWindow);
    ImGui::SameLine();
    ImGui::Checkbox("Packets", &g_showEventsWindow);
    if (ImGui::Button("Open all")) {
        g_showSdkWindow = true;
        g_showEventsWindow = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close all")) {
        g_showSdkWindow = false;
        g_showEventsWindow = false;
    }
    if (instance == nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled("No activity selected");
    }
}

} // namespace

/** Draws the Activity Host instance selector and its window launcher. */
void draw() noexcept {
    ImGui::PushID("activity_host_panel");
    host::snapshot(g_snapshot);
    select_default();
    section::header("Activity Host", nullptr);
    draw_instance_selector();
    ImGui::Spacing();
    const host::InstanceSnapshot* instance = selected_instance();
    draw_launcher(instance);
    ImGui::PopID();
}

/** Draws enabled Activity Host companion windows for every visible UI frame. */
void draw_windows() noexcept {
    host::snapshot(g_snapshot);
    select_default();
    const host::InstanceSnapshot* instance = selected_instance();
    draw_sdk_window(instance);
    event_view::draw(g_showEventsWindow, instance, g_snapshot);
}

} // namespace sunrise::server::ui::activity_host
