#include "core_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string_view>

#include "../../client/runtime/host/game_host_classification.h"
#include "../../client/runtime/runtime.h"
#include "../../middleware/runtime/middleware_runtime.h"
#include "../../server/runtime/server_runtime.h"
#include "../../state/account/account_platform.h"
#include "../../state/activity_sdk/generated_world/catalog_manifest.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/content_manifest/content_manifest_state_runtime.h"
#include "../../state/entitlements/entitlement_runtime.h"
#include "../../state/investment/store.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_runtime.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "../settings/settings.h"
#include "../ui/modules/hud/hud.h"
#include "../ui/modules/logs/logs.h"
#include "../ui/modules/registry/ui_module_registry.h"
#include "../ui/runtime/ui_visibility_runtime.h"
#include "core/threading/srw_lock.h"

namespace sunrise::core {
namespace {

std::atomic_bool g_initialized{false};
threading::SrwLock g_runtimeLock{};

/** The installed public package headers live beside the game executable. */
constexpr std::wstring_view kInstalledPackagesDirectory = L"packages";

/** Initializes the base State before content-authenticated generated artifacts are considered. */
[[nodiscard]] bool initialize_state(void* module) noexcept {
    if (!state::investment::store::initialize(module)) {
        return false;
    }
    return state::investment::store::bind_identity(
               state::account::platform::declared_account_soid(settings::get().steam.user.steamId))
           && state::investment::store::validate()
           && state::initialize(module, settings::get().initialActivityDefaults);
}

/** Copies the live public installed-content fingerprint without retaining State-owned memory. */
[[nodiscard]] bool copy_content_fingerprint(void* opaque,
                                            const state::content_manifest::View& view) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& output = *static_cast<state::activity_sdk::identity::Digest*>(opaque);
    std::copy(view.buildFingerprint.begin(), view.buildFingerprint.end(), output.begin());
    return state::activity_sdk::identity::valid(output);
}

/**
 * Authenticates the catalog committed last for this source and derives its required pack identity.
 */
[[nodiscard]] bool activity_sdk_identity(void* module,
                                         state::activity_sdk::ExpectedIdentity& output) noexcept {
    namespace manifest = state::activity_sdk::generated_world::manifest;
    output = {};
    state::activity_sdk::identity::Digest source{};
    path::Buffer catalogPath;
    if (!state::content_manifest::visit_snapshot(&copy_content_fingerprint, &source)
        || !path::module_directory(module, catalogPath)
        || !path::append(catalogPath, L"Sunrise\\sdk\\catalog.bin")) {
        return false;
    }
    manifest::Catalog catalog{};
    manifest::LoadStatus status = manifest::LoadStatus::invalid;
    return manifest::load(catalogPath.chars.data(), source, catalog, status)
           && status == manifest::LoadStatus::loaded
           && state::activity_sdk::identity::derive(source, catalog.sdk.payloadSha256, output)
           && output.sdkBuildSha256 == catalog.sdk.buildSha256;
}

/** Publishes the optional pack only through the authenticated catalog committed beside it. */
void initialize_activity_sdk(void* module) noexcept {
    state::activity_sdk::ExpectedIdentity expected{};
    (void)activity_sdk_identity(module, expected);
    state::activity_sdk::initialize(module, expected);
    // The wire catalog borrows the published catalog's strings, so it follows every publish.
}

/**
 * Builds the local content manifest only for the production game host.
 * @param module Loaded Sunrise module used for generated cache placement.
 * @return True when the host needs no manifest or one complete catalog is ready.
 */
[[nodiscard]] bool initialize_content_manifest(void* module) noexcept {
    if (client::runtime::host::current_requirement()
        == client::runtime::host::NetworkRequirement::notApplicable) {
        initialize_activity_sdk(module);
        return true;
    }
    const HMODULE process = GetModuleHandleW(nullptr);
    path::Buffer packages;
    const bool located = process != nullptr && path::module_directory(process, packages)
                         && path::append(packages, kInstalledPackagesDirectory);
    if (!located) {
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
    } else {
        initialize_activity_sdk(module);
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
    const std::lock_guard lock(g_runtimeLock);
    if (g_initialized.load(std::memory_order_relaxed)) {
        return true;
    }
    // Taken before the first stage, so the reported duration covers settings and the sinks too.
    const std::uint64_t startedTick = GetTickCount64();

    if (!settings::initialize(module)) {
        // Settings name their own failure; the sinks do not exist yet to carry a second line.
        return false;
    }
    // One stage per step, so a boot failure names the step instead of the whole expression.
    const char* stage = nullptr;
    if (!log::initialize(module, settings::get().logging)) {
        stage = "logging";
    } else {
        // The sinks exist only from here, so this is the earliest a begin marker can reach a
        // channel. The duration it pairs with still counts from function entry.
        log::write(log::Channel::core, log::Level::debug, "ev=initialize phase=begin");
        if (!ui::runtime::initialize(settings::get().client.userInterface)) {
            stage = "ui";
        } else if (!ui::modules::hud::initialize(module)) {
            // Registered before logs, which is the order the menu lists the Core pages in.
            stage = "ui_hud";
        } else if (!ui::modules::logs::initialize()) {
            stage = "ui_logs";

        } else if (!initialize_state(module)) {
            stage = "state";
        } else if (!initialize_content_manifest(module)) {
            stage = "content_manifest";
        } else if (!middleware::initialize()) {
            stage = "middleware";
        } else if (!server::initialize()) {
            stage = "server";
        } else if (settings::activates_client_hooks(settings::role())
                   && !client::initialize(module)) {
            stage = "client";
        }
    }
    if (stage != nullptr) {
        report_stage_failure(stage);
        // Reported before the unwind, so this measures initialization alone and stays comparable
        // with the success line. The unwind's own quiesce waits would otherwise be counted here.
        // A logging-stage failure has no sinks left to carry it, and reports nothing.
        log::write_elapsed(log::Channel::core, "ev=initialize phase=complete", startedTick, "fail");
        // Reverse every stage because the failing expression may have completed earlier stages.
        if (settings::activates_client_hooks(settings::role())) {
            (void)client::shutdown();
        }
        server::shutdown();
        middleware::shutdown();
        state::content_manifest::shutdown();
        state::activity_sdk::shutdown();
        state::shutdown();
        ui::modules::logs::shutdown();
        ui::modules::hud::shutdown();
        ui::modules::registry::shutdown();
        ui::runtime::shutdown();
        log::shutdown();
        settings::shutdown();
        return false;
    }
    g_initialized.store(true, std::memory_order_release);
    log::write(log::Channel::core, log::Level::info, "ev=initialize result=ok");
    log::write_elapsed(log::Channel::core, "ev=initialize phase=complete", startedTick, "ok");
    return true;
}

/** Stops every runtime layer in reverse dependency order. */
bool shutdown() noexcept {
    const std::lock_guard lock(g_runtimeLock);
    if (!g_initialized.load(std::memory_order_acquire)) {
        return true;
    }
    if (settings::activates_client_hooks(settings::role()) && !client::shutdown()) {
        // Server and State must remain valid while any Client hook is attached.
        return false;
    }
    g_initialized.store(false, std::memory_order_release);
    server::shutdown();
    middleware::shutdown();
    state::content_manifest::shutdown();
    state::activity_sdk::shutdown();
    state::shutdown();
    ui::modules::logs::shutdown();
    ui::modules::hud::shutdown();
    ui::modules::registry::shutdown();
    ui::runtime::shutdown();
    log::write(log::Channel::core, log::Level::info, "ev=shutdown result=ok");
    log::shutdown();
    settings::shutdown();
    return true;
}

/** @return True after every Core-owned runtime layer initializes. */
bool is_initialized() noexcept {
    return g_initialized.load(std::memory_order_acquire);
}

} // namespace sunrise::core
