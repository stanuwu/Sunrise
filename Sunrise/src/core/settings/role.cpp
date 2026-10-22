#include "role.h"

#include <atomic>

namespace sunrise::core::settings {

bool parse_role(std::string_view text, Role& output) noexcept {
    if (text == "embedded") {
        output = Role::embedded;
    } else if (text == "client") {
        output = Role::client;
    } else if (text == "host") {
        output = Role::host;
    } else {
        return false;
    }
    return true;
}

namespace {
/** Solo play until settings select a co-op role. */
std::atomic<Role> g_configured{Role::embedded};
} // namespace

Role role() noexcept {
    return g_configured.load(std::memory_order_acquire);
}

bool configure_role(Role value, bool specified) noexcept {
    if (!specified) {
        g_configured.store(Role::embedded, std::memory_order_release);
        return true;
    }
    if (value == Role::invalid) {
        return false;
    }
    g_configured.store(value, std::memory_order_release);
    return true;
}

} // namespace sunrise::core::settings
