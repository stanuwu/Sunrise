#include "ui_hud_mission_script_overlay.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string_view>

#include "../../../../server/activity/mission/mission_script_runtime.h"

namespace sunrise::core::ui::hud::overlays::mission_script {
namespace {

namespace mission = server::activity::mission;

/** The HUD stays compact; the Script window owns the full retained view. */
constexpr std::size_t kVisibleRows = 4;
mission::DiagnosticsSnapshot g_snapshot{};

/** @return One short word for what the VM is doing. */
[[nodiscard]] const char* vm_state(const mission::InstanceDiagnostics& instance) noexcept {
    if (instance.vmFaulted) {
        return "faulted";
    }
    if (std::string_view{instance.programStatus.data()} != "loaded") {
        return instance.programStatus.data();
    }
    return instance.vmActive ? "running" : "idle";
}

/** Draws the controller file of the activity, so the HUD names what is running. */
void draw_controller_file(const std::array<char, mission::kControllerFileCapacity>& file) noexcept {
    if (file[0] != '\0') {
        ImGui::TextDisabled("%s", file.data());
    }
}

/** Draws the newest attach refusal, which is why no script is attached. */
void draw_last_attach() noexcept {
    if (g_snapshot.attachCount == 0) {
        ImGui::TextDisabled("no script attached");
        return;
    }
    const mission::AttachDiagnostics& attach = g_snapshot.attaches[g_snapshot.attachCount - 1];
    const std::string_view result(attach.result.data());
    const std::string_view detail(attach.detail.data());
    if (detail.empty() || detail == result) {
        ImGui::TextDisabled("attach %s", attach.result.data());
    } else {
        ImGui::TextDisabled("attach %s %s", attach.result.data(), attach.detail.data());
    }
    // A refusal that still reached an exact activity names the file that would have run.
    draw_controller_file(attach.controllerFile);
}

} // namespace

/** Draws what the mission script VM is doing inside the current HUD window. */
void draw() noexcept {
    mission::snapshot(g_snapshot);
    if (!g_snapshot.enabled) {
        ImGui::TextDisabled("mission scripts off");
        return;
    }
    if (!g_snapshot.pathReady) {
        ImGui::TextDisabled("script folder missing");
        return;
    }
    if (g_snapshot.instanceCount == 0) {
        draw_last_attach();
        return;
    }
    const std::size_t rows = (std::min)(g_snapshot.instanceCount, kVisibleRows);
    for (std::size_t index = 0; index < rows; ++index) {
        const mission::InstanceDiagnostics& instance = g_snapshot.instances[index];
        ImGui::Text("%s  %s  phase %u",
                    instance.activityId[0] != '\0' ? instance.activityId.data() : "unnamed",
                    vm_state(instance),
                    instance.phase);
        draw_controller_file(instance.controllerFile);
        ImGui::TextDisabled("events %llu/%llu  effects %zu/%llu",
                            static_cast<unsigned long long>(instance.eventsCommitted),
                            static_cast<unsigned long long>(instance.eventsSeen),
                            instance.pendingIntents,
                            static_cast<unsigned long long>(instance.intentsTransportStaged));
        if (instance.vmFaulted && instance.lastVmError[0] != '\0') {
            ImGui::TextDisabled("%s", instance.lastVmError.data());
        }
    }
    if (g_snapshot.instanceCount > rows) {
        ImGui::TextDisabled("%zu more attached", g_snapshot.instanceCount - rows);
    }
}

} // namespace sunrise::core::ui::hud::overlays::mission_script
