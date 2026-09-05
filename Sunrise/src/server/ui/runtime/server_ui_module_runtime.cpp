#include "server_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../activity_host/activity_host_panel.h"
#include "../activity_override/activity_override_panel.h"

namespace sunrise::server::ui::runtime {
namespace {

/** A namespaced stable ID keeps Server modules from clashing with Client modules. */
constexpr std::string_view kOverrideStableId = "server.activity_override";
/** Short menu label for the activity override page. */
constexpr std::string_view kOverrideDisplayName = "Activity";
/** A namespaced stable ID for the launched World / SDK tool. */
constexpr std::string_view kWorldStableId = "server.world";
/** Short workspace-tab label for the World / SDK tool. */
constexpr std::string_view kWorldDisplayName = "World";

core::ui::modules::registry::PageRegistration g_overridePage;
core::ui::modules::registry::PageRegistration g_hostPage;
core::ui::modules::registry::PageRegistration g_worldPage;
core::ui::modules::registry::PageRegistration g_packetsPage;

} // namespace

/** @return True when the Server module owns its Core UI registry slot. */
bool initialize() noexcept {
    if (!g_overridePage.acquire(core::ui::modules::Owner::server,
                                kOverrideStableId,
                                kOverrideDisplayName,
                                &activity_override::draw)) {
        return false;
    }
    if (!g_hostPage.acquire(core::ui::modules::Owner::server,
                            "server.activity_host",
                            "Activity Host",
                            &activity_host::draw)) {
        g_overridePage.release();
        return false;
    }
    if (!g_worldPage.acquire(core::ui::modules::Owner::server,
                             kWorldStableId,
                             kWorldDisplayName,
                             &activity_host::draw_world,
                             nullptr,
                             nullptr,
                             &activity_host::deactivate_world,
                             core::ui::modules::Presentation::workspaceTab)) {
        g_hostPage.release();
        g_overridePage.release();
        return false;
    }
    if (!g_packetsPage.acquire(core::ui::modules::Owner::server,
                               "server.packets",
                               "Packets",
                               &activity_host::draw_packets,
                               nullptr,
                               nullptr,
                               nullptr,
                               core::ui::modules::Presentation::workspaceTab)) {
        g_worldPage.release(&activity_host::deactivate_world);
        g_hostPage.release();
        g_overridePage.release();
        return false;
    }
    return true;
}

/** Removes the Server module from the Core UI registry. */
void shutdown() noexcept {
    g_packetsPage.release();
    g_worldPage.release(&activity_host::deactivate_world);
    g_hostPage.release();
    g_overridePage.release();
}

} // namespace sunrise::server::ui::runtime
