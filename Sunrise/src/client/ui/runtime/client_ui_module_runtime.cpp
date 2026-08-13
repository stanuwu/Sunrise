#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../teleport/teleport_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable ID prevents Client modules from colliding with Server modules. */
constexpr std::string_view kTeleportStableId = "client.teleport";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kTeleportDisplayName = "Movement";

core::ui::modules::registry::PageRegistration g_teleportPage;

} // namespace

/** @return True when the Client module owns its Core UI registry slot. */
bool initialize() noexcept {
    return g_teleportPage.acquire(
        core::ui::modules::Owner::client, kTeleportStableId, kTeleportDisplayName, &teleport::draw);
}

/** Removes the Client module from the Core UI registry. */
void shutdown() noexcept {
    g_teleportPage.release();
}

} // namespace sunrise::client::ui::runtime
