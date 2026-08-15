#include "banner_hook_lifecycle.h"

#include <atomic>

#include "banner_bind.h"

namespace sunrise::client::hooks::banner {
namespace {

std::atomic_bool g_installed{false};

} // namespace

/** Attaches the banner identity bind. */
bool install() noexcept {
    const bool bind = install_banner_bind();
    g_installed.store(bind, std::memory_order_release);
    return bind;
}

/** Detaches the banner chain, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_banner_bind();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while the banner bind is attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::banner
