#include "activity_host_sdk_view.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <span>
#include <utility>

#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../core/ui/components/card/ui_card_component.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../state/activity_sdk/generation/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../activity/host_runtime.h"
#include "../../activity/mission/mission_script_runtime.h"
#include "../../bap/runtime.h"
#include "activity_host_scriptable_browser.h"
#include "activity_host_sdk_mission_view.h"
#include "activity_host_sdk_squad_view.h"
#include "activity_host_sdk_state_pages.h"
#include "activity_host_sdk_symbol_pages.h"
#include "activity_host_tool_window.h"

namespace sunrise::server::ui::activity_host::sdk_view {
namespace {

namespace card = core::ui::components::card;
namespace format = state::activity_sdk::format;
namespace generation = state::activity_sdk::generation;
namespace host = server::activity::host;
namespace marker = client::ui::activity::authored_placement_marker;
namespace mission = server::activity::mission;
namespace sdk = state::activity_sdk;
namespace scaling = core::ui::scaling::dpi;

/** Width of the navigation column, which fits the longest page name with room to spare. */
constexpr float kNavigationWidth = 132.0F;
bool g_scriptReloadRequested{};
bool g_scriptReloadAccepted{};

/** @return A catalog row count bounded for Dear ImGui's signed clipper. */
[[nodiscard]] int clipped_count(std::size_t count) noexcept {
    return static_cast<int>((std::min)(count, static_cast<std::size_t>(INT_MAX)));
}

/** Draws the selected activity's Lua attach, VM, delivery, and Host-lane health. */
void draw_script_runtime(const host::InstanceSnapshot& hostInstance) noexcept {
    mission::DiagnosticsSnapshot runtime{};
    mission::snapshot(runtime);
    ImGui::Text("Runtime %s", runtime.enabled ? "on" : "off");
    ImGui::SameLine();
    ImGui::TextDisabled("folder %s", runtime.pathReady ? "ready" : "unavailable");

    std::array<char, 260> controller{};
    const mission::InstanceDiagnostics* selected = nullptr;
    for (std::size_t index = 0; index < runtime.instanceCount; ++index) {
        if (same_binding(runtime.instances[index].binding, hostInstance.binding)) {
            selected = &runtime.instances[index];
            break;
        }
    }
    const mission::AttachDiagnostics* attach = nullptr;
    for (std::size_t index = 0; index < runtime.attachCount; ++index) {
        if (same_binding(runtime.attaches[index].binding, hostInstance.binding)) {
            attach = &runtime.attaches[index];
            break;
        }
    }
    const std::uint32_t activityRow = selected != nullptr ? selected->activityRow
                                      : attach != nullptr ? attach->activityRow
                                                          : 0;
    if (activityRow != 0 && mission::controller_file_name(activityRow, controller)) {
        ImGui::Text("File Sunrise/scripts/%s", controller.data());
    }
    if (attach != nullptr) {
        ImGui::Text("Attach %s", attach->result.data());
        if (attach->detail[0] != '\0') {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", attach->detail.data());
        }
    } else {
        ImGui::TextDisabled("No attach result yet");
    }

    ImGui::SeparatorText("VM");
    if (selected == nullptr) {
        ImGui::TextDisabled("No mission VM instance");
    } else {
        ImGui::Text("Program %s", selected->programStatus.data());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s",
                            selected->vmActive ? "active" : "inactive",
                            selected->vmFaulted ? " faulted" : "");
        ImGui::Text("Last %s: %s", selected->lastVmStage.data(), selected->lastVmStatus.data());
        if (selected->lastVmError[0] != '\0') {
            ImGui::TextWrapped("Error %s", selected->lastVmError.data());
        }
        ImGui::Text("Callbacks %llu committed %llu refused %llu",
                    static_cast<unsigned long long>(selected->vmCallbacks),
                    static_cast<unsigned long long>(selected->vmCommittedCallbacks),
                    static_cast<unsigned long long>(selected->vmRefusedCallbacks));
        ImGui::Text("Events %llu/%llu pending %zu",
                    static_cast<unsigned long long>(selected->eventsCommitted),
                    static_cast<unsigned long long>(selected->eventsSeen),
                    selected->pendingEvents);
        ImGui::Text("Delivery %s intents %zu attempts %u",
                    selected->deliveryStage.data(),
                    selected->pendingIntents,
                    selected->intentAttempts);
    }

    ImGui::SeparatorText("Host output lane");
    ImGui::Text("Status %s", host::output_status_name(hostInstance.outputStatus));
    ImGui::SameLine();
    ImGui::TextDisabled("kind %u attempts %u",
                        static_cast<unsigned>(hostInstance.outputKind),
                        hostInstance.outputAttempts);
    ImGui::Text("Queued %s", hostInstance.outputPending ? "yes" : "no");

    if (ImGui::Button("Reload script")) {
        g_scriptReloadRequested = true;
        g_scriptReloadAccepted = mission::reload();
    }
    if (g_scriptReloadRequested) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_scriptReloadAccepted ? "reload queued" : "runtime unavailable");
    }
}

/** Resolves and revalidates one panel selection against the live ActivityClient. */
[[nodiscard]] sdk::Status bind(const host::InstanceSnapshot& instance,
                               sdk::Snapshot catalog,
                               sdk::BoundView& output) noexcept {
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(instance.binding, link);
    const sdk::Selection selection{
        instance.binding, link.matchingLinks, link.activityClientGeneration};
    const sdk::Status result = sdk::resolve(std::move(catalog), selection, output);
    if (result != sdk::Status::ready) {
        return result;
    }
    server::bap::ActivityLinkView current{};
    (void)server::bap::activity_link_view(instance.binding, current);
    return sdk::revalidate(
        output, instance.binding, current.matchingLinks, current.activityClientGeneration);
}

/** One page. Each holds one family of rows and only that family's actions. */
enum class Page : std::uint8_t {
    squads,
    idles,
    combatants,
    devices,
    triggers,
    objects,
    scenes,
    dialogue,
    directives,
    objectives,
    cinematics,
    engagement,
    event,
    occupancy,
    lifetime,
    states,
    missionState,
    positions,
    behaviors,
    script,
};

/** One navigation row: its page, its name, and the one line that page prints. */
struct PageRow final {
    Page page{};
    const char* name{};
    const char* summary{};
    marker::WorldPage world{marker::WorldPage::none};
};

/** The navigation order. Actions first, then the read-only pages. */
constexpr std::array<PageRow, 20> kPages{{
    {Page::squads, "Squads", "Place an authored squad.", marker::WorldPage::squads},
    {Page::idles, "Idles", "Start an actor's authored state.", marker::WorldPage::squads},
    {Page::combatants, "Combatants", "Bind actors, edit channels and try animation sequences."},
    {Page::devices, "Devices", "Drive a door, lift or switch.", marker::WorldPage::devices},
    {Page::triggers, "Triggers", "Fire an authored pulse.", marker::WorldPage::triggers},
    {Page::objects, "Objects", "Spawn or remove a placed object.", marker::WorldPage::objects},
    {Page::scenes, "Scenes", "Activate an authored scene."},
    {Page::dialogue, "Dialogue", "Play an authored cue."},
    {Page::directives, "Directives", "Show or clear a HUD directive."},
    {Page::objectives, "Objectives", "Reset objectives, advance a task."},
    {Page::cinematics, "Cinematics", "Play a sequence or cinematic."},
    {Page::engagement, "Engagement", "Set the encounter engagement body."},
    {Page::event, "Public event", "Watch one player against an event area."},
    {Page::occupancy, "Occupancy", "Count an object filter into Sense."},
    {Page::lifetime, "Lifetime", "Set the activity lifetime state."},
    {Page::states, "States", "Select one authored state by region."},
    {Page::missionState, "Mission state", "Durable variables and timers."},
    {Page::positions,
     "Positions",
     "Package positions no slot claims.",
     marker::WorldPage::positions},
    {Page::behaviors, "Behaviors", "Compiled behavior roots. No action."},
    {Page::script, "Script", "Mission VM state and its output lane."},
}};

/** Page the navigation is on. It survives a rebind so an operator keeps their place. */
Page g_page{Page::squads};

/** Draws the navigation column and returns the page it leaves selected. */
[[nodiscard]] const PageRow& draw_navigation() noexcept {
    const float width = scaling::pixels(kNavigationWidth);
    if (ImGui::BeginChild("##activity_sdk_nav", {width, 0.0F}, ImGuiChildFlags_Borders)) {
        for (const PageRow& row : kPages) {
            if (ImGui::Selectable(row.name, row.page == g_page)) {
                g_page = row.page;
            }
        }
    }
    ImGui::EndChild();
    for (const PageRow& row : kPages) {
        if (row.page == g_page) {
            return row;
        }
    }
    return kPages.front();
}

/** Draws the one selected page inside the frame the navigation left. */
void draw_page(const PageRow& row,
               const sdk::BoundView& view,
               const format::Scenario& scenario,
               const host::InstanceSnapshot& instance) noexcept {
    switch (row.page) {
    case Page::squads:
        sdk_squad_view::draw(view, scenario);
        return;
    case Page::idles:
        sdk_state_pages::draw_performances(view);
        return;
    case Page::combatants:
        sdk_state_pages::draw_combatants(view);
        return;
    case Page::devices:
        scriptable_browser::draw_devices(&instance);
        return;
    case Page::triggers:
        scriptable_browser::draw_triggers(&instance);
        return;
    case Page::objects:
        scriptable_browser::draw_objects(&instance);
        return;
    case Page::scenes:
        sdk_mission_view::draw_scenes(view);
        return;
    case Page::dialogue:
        sdk_mission_view::draw_dialogue(view);
        return;
    case Page::directives:
        sdk_mission_view::draw_directives(view);
        return;
    case Page::objectives:
        sdk_mission_view::draw_objectives(view);
        return;
    case Page::cinematics:
        sdk_mission_view::draw_cinematics(view);
        return;
    case Page::engagement:
        sdk_state_pages::draw_engagement(view);
        return;
    case Page::event:
        sdk_state_pages::draw_public_events(view);
        return;
    case Page::occupancy:
        sdk_state_pages::draw_occupancy(view);
        return;
    case Page::lifetime:
        sdk_state_pages::draw_lifetime(view);
        return;
    case Page::states:
        sdk_state_pages::draw_states(view);
        return;
    case Page::missionState:
        sdk_state_pages::draw_mission_state(view);
        return;
    case Page::positions:
        scriptable_browser::draw_positions(&instance);
        return;
    case Page::behaviors:
        sdk_mission_view::draw_compiled_behaviors(view);
        return;
    case Page::script:
        draw_script_runtime(instance);
        return;
    }
}

/** Draws only data from one resolved immutable generated SDK view. */
void draw_bound(const sdk::BoundView& view,
                const format::Scenario& scenario,
                const host::InstanceSnapshot& instance) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const auto occurrences = sdk::scenario_occurrences(catalog, scenario);
    sync_selection(view, occurrences);
    const PageRow& row = draw_navigation();
    marker::set_world_page(row.world);
    ImGui::SameLine();
    if (ImGui::BeginChild("##activity_sdk_page", {0.0F, 0.0F})) {
        ImGui::TextDisabled("%s", row.summary);
        ImGui::PushID(row.name);
        draw_page(row, view, scenario, instance);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

/** Draws one compact line for the generator and the installed pack. */
void draw_status_line(sdk::Status loadStatus) noexcept {
    const generation::Snapshot generationState = generation::snapshot();
    ImGui::Text("SDK %s", sdk::status_name(loadStatus));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", generation::status_name(generationState.status));
    if (generationState.total != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%u/%u",
                            static_cast<unsigned>(generationState.current),
                            static_cast<unsigned>(generationState.total));
    }
    if (generationState.detail[0] != '\0') {
        if (generationState.status == generation::Status::failed) {
            ImGui::Text("Generation failed: %s", generationState.detail.data());
        } else {
            ImGui::TextDisabled("%s", generationState.detail.data());
        }
    }
}

/** Draws loader and exact-binding refusal status, then the resolved SDK. */
void draw_content(const host::InstanceSnapshot* instance) noexcept {
    scriptable_browser::prepare(instance);
    const sdk::Status loadStatus = sdk::status();
    draw_status_line(loadStatus);
    const sdk::Snapshot catalog = sdk::snapshot();
    if (loadStatus != sdk::Status::ready) {
        ImGui::TextDisabled("No activity SDK is published.");
        ImGui::SeparatorText("World objects");
        marker::set_world_page(marker::WorldPage::objects);
        scriptable_browser::draw_objects(instance);
        return;
    }
    if (!catalog) {
        ImGui::Text("SDK resolve  %s", sdk::status_name(sdk::Status::catalogInvalid));
        ImGui::SeparatorText("World objects");
        marker::set_world_page(marker::WorldPage::objects);
        scriptable_browser::draw_objects(instance);
        return;
    }
    if (instance == nullptr) {
        ImGui::TextDisabled("No activity selected.");
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    sdk::BoundView view{};
    const sdk::Status resolveStatus = bind(*instance, catalog, view);
    if (resolveStatus != sdk::Status::ready) {
        ImGui::Text("Bind refused  %s", sdk::status_name(resolveStatus));
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    const format::Activity* const activity = sdk::bound_activity(view);
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (activity == nullptr || scenario == nullptr) {
        ImGui::Text("Bind refused  %s", sdk::status_name(sdk::Status::catalogInvalid));
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    draw_bound(view, *scenario, *instance);
}

} // namespace

/** Draws the generated activity SDK and its proved Activity Host actions. */
void draw(bool& open, const host::InstanceSnapshot* instance) noexcept {
    if (!open) {
        marker::set_world_page(marker::WorldPage::none);
        return;
    }
    const bool visible = tool_window::begin(
        "Activity Host - World###activity_host_sdk", open, {32.0F, 40.0F}, {940.0F, 680.0F});
    if (visible) {
        const card::Scope surface("##activity_host_sdk_card");
        if (surface.visible()) {
            draw_content(instance);
        }
    }
    ImGui::End();
    if (!open) {
        marker::set_world_page(marker::WorldPage::none);
    }
}

} // namespace sunrise::server::ui::activity_host::sdk_view
