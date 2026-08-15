#include "core_runtime.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <string_view>

#include "../../client/runtime/host/game_host_classification.h"
#include "../../client/runtime/runtime.h"
#include "../../middleware/runtime/middleware_runtime.h"
#include "../../server/runtime/server_runtime.h"
#include "../../state/content_manifest/content_manifest_state_runtime.h"
#include "../../state/entitlements/entitlement_runtime.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_runtime.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "../settings/settings.h"
#include "../ui/modules/logs/logs.h"
#include "../ui/modules/registry/ui_module_registry.h"
#include "../ui/runtime/ui_visibility_runtime.h"

namespace sunrise::core {
namespace {

std::atomic_bool g_initialized{false};
SRWLOCK g_runtimeLock{SRWLOCK_INIT};

/** The installed public package headers live beside the game executable. */
constexpr std::wstring_view kInstalledPackagesDirectory = L"packages";

/**
 * Builds the local content manifest only for the production game host.
 * @param module Loaded Sunrise module used for generated cache placement.
 * @return True when the host needs no manifest or one complete catalog is ready.
 */
[[nodiscard]] bool initialize_content_manifest(void* module) noexcept {
    if (client::runtime::host::current_requirement()
        == client::runtime::host::NetworkRequirement::notApplicable) {
        return true;
    }
    const HMODULE process = GetModuleHandleW(nullptr);
    path::Buffer packages;
    if (process == nullptr || !path::module_directory(process, packages)
        || !path::append(packages, kInstalledPackagesDirectory)) {
        log::write(log::Channel::core,
                   log::Level::error,
                   "ev=initialize stage=content_manifest result=fail");
        return false;
    }
    const bool initialized = state::content_manifest::initialize(
        module, std::wstring_view(packages.chars.data(), packages.length));
    if (!initialized) {
        log::write(log::Channel::core,
                   log::Level::error,
                   "ev=initialize stage=content_manifest result=fail");
    }
    return initialized;
}

/**
 * Names the initialization stage that failed.
 * The logging sinks are the first stage, so their own failure has only the debugger.
 * @param stage Short key naming the stage.
 */
void report_stage_failure(const char* stage) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=initialize stage=%s result=fail", stage);
    if (written <= 0) {
        return;
    }
    const std::string_view event{line.data(), static_cast<std::size_t>(written)};
    log::early(event);
    log::write(log::Channel::core, log::Level::error, event);
}

} // namespace

/** Initializes every runtime layer in dependency order. */
bool initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_runtimeLock);
    if (g_initialized.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&g_runtimeLock);
        return true;
    }

    if (!settings::initialize(module)) {
        // Settings name their own failure; the sinks do not exist yet to carry a second line.
        ReleaseSRWLockExclusive(&g_runtimeLock);
        return false;
    }
    state::unlocks::publish(settings::get().initialUnlocks);
    // One stage per step, so a boot failure names the step instead of the whole expression.
    const char* stage = nullptr;
    if (!log::initialize(module, settings::get().logging)) {
        stage = "logging";
    } else if (!ui::runtime::initialize(settings::get().client.userInterface)) {
        stage = "ui";
    } else if (!ui::modules::logs::initialize()) {
        stage = "ui_logs";
    } else if (!state::entitlements::publish(settings::get().server.entitlements)) {
        stage = "entitlements";
    } else if (!state::initialize(module,
                                  settings::get().initialAccount,
                                  settings::get().initialActivityDefaults)) {
        stage = "state";
    } else if (!initialize_content_manifest(module)) {
        stage = "content_manifest";
    } else if (!middleware::initialize()) {
        stage = "middleware";
    } else if (!server::initialize()) {
        stage = "server";
    } else if (!client::initialize(module)) {
        stage = "client";
    }
    if (stage != nullptr) {
        report_stage_failure(stage);
        // Reverse every stage because the failing expression may have completed earlier stages.
        (void)client::shutdown();
        server::shutdown();
        middleware::shutdown();
        state::content_manifest::shutdown();
        state::shutdown();
        state::entitlements::clear();
        ui::modules::logs::shutdown();
        ui::modules::registry::shutdown();
        ui::runtime::shutdown();
        state::unlocks::clear();
        log::shutdown();
        settings::shutdown();
        ReleaseSRWLockExclusive(&g_runtimeLock);
        return false;
    }
    g_initialized.store(true, std::memory_order_release);
    log::write(log::Channel::core, log::Level::info, "ev=initialize result=ok");
    ReleaseSRWLockExclusive(&g_runtimeLock);
    return true;
}

/** Stops every runtime layer in reverse dependency order. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_runtimeLock);
    if (!g_initialized.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&g_runtimeLock);
        return true;
    }
    if (!client::shutdown()) {
        // Server and State must remain valid while any Client hook is attached.
        ReleaseSRWLockExclusive(&g_runtimeLock);
        return false;
    }
    g_initialized.store(false, std::memory_order_release);
    server::shutdown();
    middleware::shutdown();
    state::content_manifest::shutdown();
    state::shutdown();
    state::entitlements::clear();
    ui::modules::logs::shutdown();
    ui::modules::registry::shutdown();
    ui::runtime::shutdown();
    log::write(log::Channel::core, log::Level::info, "ev=shutdown result=ok");
    state::unlocks::clear();
    log::shutdown();
    settings::shutdown();
    ReleaseSRWLockExclusive(&g_runtimeLock);
    return true;
}

/** @return True after every Core-owned runtime layer initializes. */
bool is_initialized() noexcept {
    return g_initialized.load(std::memory_order_acquire);
}

} // namespace sunrise::core
