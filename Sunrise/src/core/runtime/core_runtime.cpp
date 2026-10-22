#include "core_runtime.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

#include "../../client/runtime/host/game_host_classification.h"
#include "../../client/runtime/runtime.h"
#include "../../middleware/content/packages/reader/overlay.h"
#include "../../middleware/runtime/middleware_runtime.h"
#include "../../server/runtime/server_runtime.h"
#include "../../state/account/account_platform.h"
#include "../../state/activity_sdk/generated_world/catalog_manifest.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/build_data/runtime.h"
#include "../../state/content_manifest/content_manifest_state_runtime.h"
#include "../../state/entitlements/entitlement_runtime.h"
#include "../../state/investment/store.h"
#include "../../state/mods/mods_runtime.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_runtime.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "../settings/settings.h"
#include "../ui/modules/hud/hud.h"
#include "../ui/modules/logs/logs.h"
#include "../ui/modules/mods/mods.h"
#include "../ui/modules/registry/ui_module_registry.h"
#include "../ui/notice/ui_notice_overlay.h"
#include "../ui/runtime/ui_visibility_runtime.h"
#include "core/threading/srw_lock.h"

namespace sunrise::core {
namespace {

namespace package_reader = middleware::content::packages::reader;

std::atomic_bool g_initialized{false};
threading::SrwLock g_runtimeLock{};

/** The installed public package headers live beside the game executable. */
constexpr std::wstring_view kInstalledPackagesDirectory = L"packages";

/** Opens the save before choosing its persistent or session-only write destination. */
[[nodiscard]] bool initialize_store(void* module) noexcept {
    if (!state::investment::store::initialize(module)) {
        return false;
    }
    return state::investment::store::bind_identity(
               state::account::platform::declared_account_soid(settings::get().steam.user.steamId))
           && state::investment::store::validate();
}

/**
 * Authenticates the catalog committed last for this source and derives its required pack identity.
 * @param module Loaded Sunrise module, which owns the SDK folder.
 * @param output Receives the pack identity the catalog authorizes.
 * @param reason Receives the log name of the step that refused, or stays null.
 * @return True when the catalog loaded and names a consistent pack identity.
 */
[[nodiscard]] bool activity_sdk_identity(void* module,
                                         state::activity_sdk::ExpectedIdentity& output,
                                         const char*& reason) noexcept {
    namespace manifest = state::activity_sdk::generated_world::manifest;
    output = {};
    reason = nullptr;
    state::activity_sdk::identity::Digest source{};
    path::Buffer catalogPath;
    // The SDK keys on the installed rows alone; mods never change them.
    if (!state::content_manifest::installed_fingerprint(source)
        || !state::activity_sdk::identity::valid(source)) {
        reason = "content_manifest";
        return false;
    }
    if (!path::module_directory(module, catalogPath)
        || !path::append(catalogPath, L"Sunrise\\sdk\\catalog.bin")) {
        reason = "catalog_path";
        return false;
    }
    manifest::Catalog catalog{};
    manifest::LoadStatus status = manifest::LoadStatus::invalid;
    if (!manifest::load(catalogPath.chars.data(), source, catalog, status)
        || status != manifest::LoadStatus::loaded) {
        reason = manifest::status_name(status);
        return false;
    }
    if (!state::activity_sdk::identity::derive(source, catalog.sdk.payloadSha256, output)
        || output.sdkBuildSha256 != catalog.sdk.buildSha256) {
        reason = "sdk_identity";
        return false;
    }
    return true;
}

/** Publishes the optional pack only through the authenticated catalog committed beside it. */
void initialize_activity_sdk(void* module) noexcept {
    state::activity_sdk::ExpectedIdentity expected{};
    const char* reason = nullptr;
    const bool authorized = activity_sdk_identity(module, expected, reason);
    state::activity_sdk::initialize(module, expected);
    const state::activity_sdk::Status status = state::activity_sdk::status();
    if (status != state::activity_sdk::Status::ready) {
        log::writef(log::Channel::core,
                    log::Level::info,
                    "ev=activity_sdk stage=boot_load result=rejected catalog=%s pack=%s",
                    authorized ? "loaded" : reason,
                    state::activity_sdk::status_name(status));
    }
}

/**
 * Loads installed mods and adds their packages to the manifest the client will request.
 * @param module Loaded Sunrise module, which owns the mods folder.
 * @param game Game folder, without a trailing separator.
 * @param packages Installed packages folder.
 * @return False when the mod list could not be published.
 */
[[nodiscard]] bool
initialize_mods(void* module, std::wstring_view game, std::wstring_view packages) noexcept {
    path::Buffer artifact;
    if (!path::artifact_directory(module, artifact)) {
        return false;
    }
    return state::mods::initialize(state::mods::Directories{
        game, packages, std::wstring_view(artifact.chars.data(), artifact.length)});
}

/**
 * Builds the local content manifest and the mod overlay only for the production game host.
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
    // The module directory keeps its trailing separator; the game folder view drops it.
    const bool resolved = process != nullptr && path::module_directory(process, packages);
    const std::size_t gameLength = resolved ? packages.length - 1 : 0;
    if (!resolved || !path::append(packages, kInstalledPackagesDirectory)
        || !state::content_manifest::initialize(
            module, std::wstring_view(packages.chars.data(), packages.length))) {
        log::write(log::Channel::core,
                   log::Level::error,
                   "ev=initialize stage=content_manifest result=fail");
        return false;
    }
    // Mods follow the installed manifest. The server reads their packages; the SDK never does.
    const std::wstring_view game(packages.chars.data(), gameLength);
    if (!initialize_mods(module, game, std::wstring_view(packages.chars.data(), packages.length))) {
        log::write(log::Channel::core, log::Level::error, "ev=initialize stage=mods result=fail");
    }
    // The build-data cache keys on the leading bytes of the catalog fingerprint, mods included.
    state::content_manifest::Fingerprint catalog{};
    std::uint64_t contentHash = 0;
    if (state::content_manifest::build_fingerprint(catalog)) {
        std::memcpy(&contentHash, catalog.data(), sizeof contentHash);
    }
    state::build_data::bind_content(contentHash);
    initialize_activity_sdk(module);
    return true;
}

/**
 * Selects the in-memory save before State can seed or canonicalize mod-dependent rows.
 * @return False when the copy failed; the boot then stops.
 */
[[nodiscard]] bool initialize_cheat_session() noexcept {
    const bool tools = settings::get().client.cheats;
    const std::size_t cheats = state::mods::cheat_count();
    if (cheats == 0 && !tools) {
        return true;
    }
    if (!state::investment::store::use_session_copy()) {
        log::writef(log::Channel::state,
                    log::Level::error,
                    "ev=mods stage=cheat result=fail mods=%zu tools=%d",
                    cheats,
                    tools ? 1 : 0);
        return false;
    }
    log::writef(log::Channel::state,
                log::Level::info,
                "ev=mods stage=cheat result=session_copy mods=%zu tools=%d",
                cheats,
                tools ? 1 : 0);
    ui::notice::raise("Cheats are on. Progress this session is not saved.");
    return true;
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
        } else if (!ui::modules::mods::initialize()) {
            stage = "ui_mods";
        } else if (!initialize_content_manifest(module)) {
            // The build-data cache in State keys on the content, mods included, so this goes first.
            stage = "content_manifest";
        } else if (!initialize_store(module)) {
            stage = "investment_store";
        } else if (!initialize_cheat_session()) {
            stage = "cheat_session";
        } else if (!state::initialize(module, settings::get().initialActivityDefaults)) {
            stage = "state";
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
        state::shutdown();
        package_reader::overlay::clear();
        state::mods::shutdown();
        state::content_manifest::shutdown();
        state::activity_sdk::shutdown();
        ui::modules::mods::shutdown();
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
    state::shutdown();
    package_reader::overlay::clear();
    state::mods::shutdown();
    state::content_manifest::shutdown();
    state::activity_sdk::shutdown();
    ui::modules::mods::shutdown();
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
