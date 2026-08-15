#include "graphics_hook_lifecycle.h"

#include <algorithm>

#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "graphics_hook_replacements.h"

namespace sunrise::client::hooks::graphics {

SRWLOCK g_hookLock{SRWLOCK_INIT};
std::atomic_uint g_activeHookCalls{};
thread_local std::size_t g_hookCallDepth{};
bool g_acceptRendererCalls{};
std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::array<void*, kHandleCount> g_targetEntries{};
Targets g_targets{};

namespace {

/** @return True when every required graphics handle is attached. */
[[nodiscard]] bool all_installed() noexcept {
    return std::all_of(g_handles.begin(),
                       g_handles.end(),
                       [](const hooking::detour::Handle& handle) { return handle.attached; });
}

/** @return True when at least one graphics handle is attached. */
[[nodiscard]] bool any_installed() noexcept {
    return std::any_of(g_handles.begin(),
                       g_handles.end(),
                       [](const hooking::detour::Handle& handle) { return handle.attached; });
}

} // namespace

/** Finds and installs both system DXGI hooks in one transaction. */
bool install() noexcept {
    const core::ui::runtime::VisibilitySnapshot visibility = core::ui::runtime::snapshot();
    if (!visibility.initialized || !visibility.enabled) {
        return visibility.initialized;
    }

    AcquireSRWLockExclusive(&g_hookLock);
    if (all_installed()) {
        ReleaseSRWLockExclusive(&g_hookLock);
        return true;
    }
    if (!hook_calls_idle() || g_acceptRendererCalls || any_installed()
        || g_targets.present != nullptr || !discovery::resolve(g_targets)) {
        ReleaseSRWLockExclusive(&g_hookLock);
        return false;
    }

    const replacement::EntryPoints entryPoints = replacement::entry_points();
    const std::array specs{
        hooking::detour::Spec{g_targets.present,
                              entryPoints[static_cast<std::size_t>(HookSlot::present)]},
        hooking::detour::Spec{g_targets.resizeBuffers,
                              entryPoints[static_cast<std::size_t>(HookSlot::resizeBuffers)]},
        hooking::detour::Spec{g_targets.setFullscreenState,
                              entryPoints[static_cast<std::size_t>(HookSlot::setFullscreenState)]},
    };
    for (std::size_t index = 0; index < specs.size(); ++index) {
        g_targetEntries[index] = specs[index].target;
    }
    const bool installed = hooking::detour::install(specs, g_handles);
    if (installed) {
        g_acceptRendererCalls = true;
    } else {
        g_targetEntries = {};
        discovery::release(g_targets);
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return installed;
}

/** Detaches system hooks before releasing renderer and discovery resources. */
bool uninstall() noexcept {
    if (current_thread_in_hook_call()) {
        // SRW locks cannot be upgraded recursively; the caller retries after the SDK call unwinds.
        return false;
    }
    AcquireSRWLockExclusive(&g_hookLock);
    if (any_installed() && !all_installed()) {
        ReleaseSRWLockExclusive(&g_hookLock);
        renderer::dispatch_pending_input_release();
        return false;
    }
    if (!any_installed()) {
        if (!hook_calls_idle() || !renderer::shutdown()) {
            ReleaseSRWLockExclusive(&g_hookLock);
            renderer::dispatch_pending_input_release();
            return false;
        }
    } else {
        g_acceptRendererCalls = false;
        if (!renderer::shutdown()) {
            // The attached Present stays available to send an owner-thread capture release.
            g_acceptRendererCalls = true;
            ReleaseSRWLockExclusive(&g_hookLock);
            renderer::dispatch_pending_input_release();
            return false;
        }

        const replacement::ProtectedEntries protectedEntries = replacement::protected_entries();
        const hooking::detour::UninstallResult removal =
            hooking::detour::uninstall(g_handles, protectedEntries);
        if (removal == hooking::detour::UninstallResult::failed) {
            // A failed transaction keeps the handles attached and the original targets callable.
            ReleaseSRWLockExclusive(&g_hookLock);
            renderer::dispatch_pending_input_release();
            return false;
        }
        if (removal == hooking::detour::UninstallResult::protectedCodeActive) {
            // Suspended ingress keeps entry closed until a later detach retry.
            ReleaseSRWLockExclusive(&g_hookLock);
            renderer::dispatch_pending_input_release();
            return false;
        }
        if (!hook_calls_idle()) {
            // Detached replacements keep the restored targets until the retry drains.
            ReleaseSRWLockExclusive(&g_hookLock);
            renderer::dispatch_pending_input_release();
            return false;
        }
    }

    if (g_acceptRendererCalls) {
        // A detached state must never publish renderer work into a later restart.
        g_acceptRendererCalls = false;
    }
    if (!hook_calls_idle()) {
        ReleaseSRWLockExclusive(&g_hookLock);
        renderer::dispatch_pending_input_release();
        return false;
    }
    g_targetEntries = {};
    discovery::release(g_targets);
    ReleaseSRWLockExclusive(&g_hookLock);
    renderer::dispatch_pending_input_release();
    return true;
}

/** @return True only when both system DXGI method hooks are attached. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_hookLock);
    const bool installed = all_installed();
    ReleaseSRWLockShared(&g_hookLock);
    return installed;
}

} // namespace sunrise::client::hooks::graphics
