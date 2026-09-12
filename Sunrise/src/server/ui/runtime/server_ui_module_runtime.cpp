#include "server_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../activity_host/activity_host_panel.h"

namespace sunrise::server::ui::runtime {
namespace {

/** A namespaced stable ID for the Activity Host page. */
constexpr std::string_view kHostStableId = "server.activity_host";
/** Short menu label for the Activity Host page. */
constexpr std::string_view kHostDisplayName = "Activity Host";

core::ui::modules::registry::PageRegistration g_hostPage;

} // namespace

/** @return True when the Server module owns its Core UI registry slot. */
bool initialize() noexcept {
    return g_hostPage.acquire(core::ui::modules::Owner::server,
                              kHostStableId,
                              kHostDisplayName,
                              &activity_host::draw,
                              nullptr,
                              &activity_host::draw_windows);
}

/** Removes the Server module from the Core UI registry. */
void shutdown() noexcept {
    g_hostPage.release();
}

} // namespace sunrise::server::ui::runtime
