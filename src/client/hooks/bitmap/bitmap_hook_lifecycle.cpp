#include "bitmap_hook_lifecycle.h"

#include <atomic>

#include "bitmap_ref_guard.h"

namespace sunrise::client::hooks::bitmap {
namespace {

std::atomic_bool g_installed{false};

} // namespace

/** Attaches the bitmap crash guards. */
bool install() noexcept {
    const bool refGuard = install_bitmap_ref_guard();
    g_installed.store(refGuard, std::memory_order_release);
    return refGuard;
}

/** Detaches every bitmap crash guard, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_bitmap_ref_guard();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while at least one bitmap crash guard is attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::bitmap
