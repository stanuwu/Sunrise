#include "event_launcher_panel.h"

#include <cstdint>
#include <imgui.h>
#include <span>

#include "../../../server/activity/mission/event_launcher.h"
#include "../../../server/bap/runtime.h"
#include "../../hooks/bootflow/bootflow_hook_lifecycle.h"

namespace sunrise::client::ui::event_launcher {
namespace {
namespace launcher = server::activity::mission::event_launcher;

/** A short rally leaves time to close the overlay and reach the flag. */
constexpr int kDefaultDelaySeconds = 30;
int g_delay = kDefaultDelaySeconds;
int g_location = -1;
int g_event{};
int g_lastRegion = -1;
std::uint64_t g_session{};
std::uint64_t g_generation{};
bool g_refused{};

const launcher::Site*
site_at(std::span<const launcher::Site> sites, int region, int ordinal = 0) noexcept {
    for (const auto& site : sites) {
        if (site.region == region && (!ordinal || site.ordinal == ordinal)) {
            return &site;
        }
    }
    return nullptr;
}
} // namespace

void draw() noexcept {
    const auto slice = hooks::bootflow::current_slice_set();
    server::bap::CurrentActivityLinkView link{};
    if (!hooks::bootflow::in_world()
        || !server::bap::current_activity_link_view(slice.present ? slice.index : -1, link)) {
        ImGui::TextDisabled("Land in a patrol destination to view its public events.");
        return;
    }
    const int here = slice.present ? slice.index : (slice.available ? -1 : link.effectiveRegion);
    launcher::Menu menu{};
    launcher::read(link.binding, here, menu);
    const auto sites = std::span(menu.sites).first(menu.count);
    if (sites.empty()) {
        ImGui::TextDisabled(
            menu.scriptFailed
                ? "The destination's mission script failed to load. See the server log."
                : "No playable event sites have loaded for this destination.");
        return;
    }
    if (g_session != link.binding.sessionId || g_generation != link.binding.createdRevision
        || g_lastRegion != here) {
        g_session = link.binding.sessionId;
        g_generation = link.binding.createdRevision;
        g_lastRegion = here;
        g_location = here;
        g_event = 0;
        g_refused = false;
    }
    const auto* location = site_at(sites, g_location);
    if (ImGui::BeginCombo("Location",
                          location ? location->location.data() : "Current area (no events)")) {
        for (const auto& site : sites) {
            if (&site != site_at(sites, site.region)) {
                continue;
            }
            ImGui::PushID(site.region);
            if (ImGui::Selectable(site.location.data(), site.region == g_location)) {
                g_location = site.region;
                g_event = 0;
                g_refused = false;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    location = site_at(sites, g_location);
    if (!location) {
        return;
    }
    if (!site_at(sites, g_location, g_event) || !g_event) {
        g_event = location->ordinal;
    }
    for (const auto& site : sites) {
        if (site.region == g_location) {
            ImGui::PushID(site.ordinal);
            ImGui::RadioButton(site.name.data(), &g_event, site.ordinal);
            ImGui::PopID();
        }
    }
    if (g_location != here) {
        ImGui::TextDisabled("Travel to this location to start an event.");
        return;
    }
    if (!menu.owner.sessionId) {
        ImGui::TextDisabled("Waiting for the local event host.");
        return;
    }
    const auto* current = site_at(sites, here, menu.currentSite);
    if (menu.currentSite && menu.phase[0]) {
        ImGui::Text("%s: %s", menu.phase.data(), current ? current->name.data() : "Public event");
        if (menu.rallySeconds > 0) {
            ImGui::Text("Starts in %lld:%02lld",
                        static_cast<long long>(menu.rallySeconds / 60),
                        static_cast<long long>(menu.rallySeconds % 60));
        }
    } else if (menu.request.serial == menu.acknowledged && !menu.manual) {
        ImGui::TextDisabled("Normal rotation");
    }
    if (menu.request.serial != menu.acknowledged && menu.request.region == here) {
        const auto* requested = site_at(sites, here, menu.request.site);
        if (menu.request.site && requested) {
            if (menu.request.rallySeconds == 0) {
                ImGui::Text("Queued: %s", requested->name.data());
            } else {
                ImGui::Text(
                    "Queued: %s (%d s rally)", requested->name.data(), menu.request.rallySeconds);
            }
        } else {
            ImGui::TextDisabled("Ending the event and cleaning up...");
        }
    }
    ImGui::InputInt("Rally countdown (seconds)", &g_delay);
    const bool validDelay = g_delay >= 0 && g_delay <= launcher::kMaxRallySeconds;
    ImGui::BeginDisabled(!validDelay);
    if (ImGui::Button(g_delay == 0 ? "Start now" : "Start rally")) {
        g_refused = !launcher::submit(menu.owner, here, g_event, g_delay);
    }
    if (menu.currentSite) {
        ImGui::SetItemTooltip("Ends the current event before starting your selection.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool queued = menu.request.site && menu.request.region == here
                        && menu.request.serial != menu.acknowledged;
    // Site zero ends whatever runs here, forced or scheduled, and drops a queued selection.
    ImGui::BeginDisabled(!queued && !menu.manual && !menu.currentSite);
    if (ImGui::Button("End event")) {
        g_refused = !launcher::submit(menu.owner, here, 0, 0);
    }
    ImGui::SetItemTooltip("Stops the event here, removes its enemies, objects, flag and HUD rows, "
                          "and returns the area to its schedule.");
    ImGui::EndDisabled();
    if (!validDelay) {
        ImGui::TextDisabled("Enter 0 to %d seconds.", launcher::kMaxRallySeconds);
    }
    if (g_refused) {
        ImGui::TextDisabled("The event host changed. Select the event again.");
    }
}
} // namespace sunrise::client::ui::event_launcher
