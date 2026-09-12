#include "activity_sdk_generation_worker.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../state/activity_sdk/generated_world/catalog_manifest.h"
#include "../../../state/activity_sdk/generated_world/codec.h"
#include "../../../state/activity_sdk/generated_world/store.h"
#include "../../../state/activity_sdk/generation/internal.h"
#include "../../../state/activity_sdk/identity.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/content_manifest/content_manifest_state_runtime.h"
#include "../items/packages/internal.h"
#include "../scenarios/scenario_build.h"
#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_generation_report.h"
#include "activity_sdk_generation_worker_internal.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_shard_cleanup.h"
#include "activity_sdk_topology_inventory.h"
#include "scriptable_catalog_builder.h"

namespace sunrise::client::content::activity::sdk_generation {
namespace {

namespace generated = state::activity_sdk::generated_world;
namespace manifest = state::activity_sdk::generated_world::manifest;
namespace shard_store = state::activity_sdk::generated_world::store;
namespace package_reader = middleware::content::packages::reader;
namespace catalog = state::build_data::scriptables;
namespace builder = ::sunrise::client::content::activity::scriptables::internal;
namespace inventory = ::sunrise::client::content::activity::sdk_generation::activity_inventory;
namespace topology = ::sunrise::client::content::activity::sdk_generation::topology_inventory;
namespace lua = ::sunrise::client::content::activity::sdk_generation::lua_artifacts;
namespace worker = worker_internal;

// Generated artifact paths below the scenario root.
constexpr std::wstring_view kSdkDirectorySuffix = L"\\sdk";
constexpr std::wstring_view kScenarioDirectorySuffix = L"\\sdk\\scenarios";
constexpr std::wstring_view kCatalogFileSuffix = L"\\sdk\\catalog.bin";
constexpr std::wstring_view kPackFileSuffix = L"\\activity_sdk.pack";
constexpr std::wstring_view kShardExtension = L".pack";
// Wait this long before probing package data again.
constexpr ULONGLONG kPackageWaitMs = 1'000;
/** A small batch bounds complete snapshots retained while parallel work finishes. */
constexpr std::size_t kScenarioBatchSize = 24;

using worker::binding_ready;
using worker::build_inventory;
using worker::build_scenarios;
using worker::build_world_source;
using worker::convert_binding_completeness;
using worker::find_record;
using worker::kDetailCapacity;
using worker::load_full_record;
using worker::load_record;
using worker::publish_estate;
using worker::publish_progress;
using worker::record_name_matches;
using worker::Scenario;
using worker::ScenarioBuildResult;
using worker::shard_path;
using worker::Work;

SRWLOCK g_lock{SRWLOCK_INIT};
bool g_configured{};
bool g_enabled{};
bool g_luaDeclarations{};
bool g_started{};
bool g_offlineActive{};
ULONGLONG g_packageReadyAfter{};
HANDLE g_thread{};
void* g_module{};
std::wstring g_sdkDirectory{};
std::wstring g_scenarioDirectory{};
std::wstring g_catalogPath{};
std::wstring g_packPath{};
std::atomic_bool g_cancel{};
SRWLOCK g_offlineLock{SRWLOCK_INIT};
OfflineCancelProbe g_offlineCancel{};
void* g_offlineCancelContext{};
OfflineProgressSink g_offlineProgress{};
void* g_offlineProgressContext{};

/** Requires a final catalog identity to be derived from its source and packed row digest. */
[[nodiscard]] bool final_sdk_identity(const generated::Digest& sourceFingerprint,
                                      const manifest::SdkIdentity& sdk) noexcept {
    state::activity_sdk::identity::Expected expected{};
    return state::activity_sdk::identity::derive(sourceFingerprint, sdk.payloadSha256, expected)
           && expected.sdkBuildSha256 == sdk.buildSha256;
}

[[nodiscard]] bool cancelled() noexcept {
    if (g_offlineCancel != nullptr && g_offlineCancel(g_offlineCancelContext)) {
        return true;
    }
    return g_cancel.load(std::memory_order_relaxed);
}

/** Emits one timing row for the two user-visible generation stages. */
void log_stage_duration(std::string_view stage,
                        ULONGLONG started,
                        std::size_t scenarios,
                        bool ready) noexcept {
    std::array<char, 192> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_sdk_generation stage=%.*s ms=%llu scenarios=%zu result=%s",
                      static_cast<int>(stage.size()),
                      stage.data(),
                      static_cast<unsigned long long>(GetTickCount64() - started),
                      scenarios,
                      ready ? "ready" : "failed");
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Copies the selected installed-content identity while the manifest owns its view. */
[[nodiscard]] bool copy_fingerprint(void* opaque,
                                    const state::content_manifest::View& view) noexcept {
    if (opaque == nullptr || view.buildFingerprint.size() != generated::Digest{}.size()) {
        return false;
    }
    auto& output = *static_cast<generated::Digest*>(opaque);
    std::copy(view.buildFingerprint.begin(), view.buildFingerprint.end(), output.begin());
    return true;
}

/** Accepts an existing directory or creates one exact owned child. */
[[nodiscard]] bool ensure_directory(const std::wstring& path) noexcept {
    if (path.empty()) {
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Accepts only a missing or empty ordinary output root for the internal offline pass. */
[[nodiscard]] bool isolated_output_ready(std::wstring_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    try {
        const std::wstring terminated(path);
        const DWORD attributes = GetFileAttributesW(terminated.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        WIN32_FIND_DATAW entry{};
        const std::wstring search = terminated + L"\\*";
        const HANDLE find = FindFirstFileW(search.c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) {
            return GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        bool empty = true;
        do {
            const std::wstring_view name(entry.cFileName);
            if (name != L"." && name != L"..") {
                empty = false;
                break;
            }
        } while (FindNextFileW(find, &entry) != FALSE);
        return FindClose(find) != FALSE && empty;
    } catch (...) {
        return false;
    }
}

/** Copies the package-reader inputs into one work item. */
[[nodiscard]] bool prepare_work(Work& work) noexcept {
    try {
        return state::content_manifest::visit_snapshot(&copy_fingerprint, &work.sourceFingerprint)
               && items::packages::collect_keys(work.keys);
    } catch (...) {
        SecureZeroMemory(&work.keys, sizeof work.keys);
        return false;
    }
}

/** Adapts the process-wide cancellation flag to the inventory builder callback. */
[[nodiscard]] bool inventory_cancelled(void*) noexcept {
    return cancelled();
}

/** Copies one terminal pass result before worker-owned storage is released. */
void finish_offline(Work& work, bool complete, std::size_t built, std::size_t reused) noexcept {
    if (!work.offline || work.offlineStatus == nullptr || work.offlineResult == nullptr) {
        return;
    }
    *work.offlineStatus =
        complete ? OfflineBuildStatus::ready
                 : (cancelled() ? OfflineBuildStatus::cancelled : OfflineBuildStatus::failed);
    *work.offlineResult = {
        static_cast<std::uint32_t>(work.scenarios.size()),
        static_cast<std::uint32_t>(work.activityRoots.size()),
        static_cast<std::uint32_t>(work.activityVariants.size()),
        static_cast<std::uint32_t>(built),
        static_cast<std::uint32_t>(reused),
        work.payloadSha256,
        work.packBytes,
        work.luaFiles,
    };
}

/** Owns the full-estate package pass until its catalog publishes or the pass fails. */
DWORD WINAPI thread_main(void* opaque) noexcept {
    std::unique_ptr<Work> work(static_cast<Work*>(opaque));
    bool complete = false;
    std::size_t built = 0;
    std::size_t reused = 0;
    std::uint32_t failureScenario = 0;
    const char* failureDetail = "pass_incomplete";
    // Outlives the shard build, because the reported detail can point into it.
    std::array<char, kDetailCapacity> detailScratch{};
    try {
        const package_reader::Source source{work->packageDirectory, &work->keys};
        publish_progress(state::activity_sdk::generation::Status::preparing,
                         0,
                         static_cast<std::uint32_t>(
                             middleware::content::packages::tables::kActivityDefinitionCount),
                         0,
                         "building installed activity inventory");
        // The game normally publishes these layouts during boot. An isolated generation
        // process has no boot worker, but container and spatial extraction still need them.
        bool layoutsReady = state::build_data::scenario_layouts_ready();
        if (work->offline && !layoutsReady) {
            auto scratch = std::make_unique<package_reader::Scratch>();
            constexpr std::size_t kMaximumLayoutSteps = 10'000;
            for (std::size_t step = 0; step < kMaximumLayoutSteps && !cancelled() && !layoutsReady;
                 ++step) {
                layoutsReady = scenarios::build(source, *scratch);
            }
            package_reader::close_files(*scratch);
        }
        bool inventoried = layoutsReady && build_inventory(*work, source);
        if (!inventoried) {
            failureDetail = layoutsReady ? "inventory_incomplete" : "scenario_layouts_unavailable";
        }

        const std::uint32_t total = static_cast<std::uint32_t>(work->scenarios.size());
        publish_progress(state::activity_sdk::generation::Status::preparing,
                         0,
                         total,
                         0,
                         "sweeping installed containers once for the whole pass");
        // The container sweep does not depend on the scenario. Sweeping it inside the loop read
        // the whole install once per scenario and kept one stem's worth of it.
        builder::ContainerIndex containers{};
        // Shards built against an empty index drop every container placement, so a failed
        // sweep must fail the pass instead of silently degrading the output.
        const bool containersReady = builder::build_container_index(source, containers, &cancelled);
        publish_progress(state::activity_sdk::generation::Status::building,
                         0,
                         total,
                         0,
                         "building generated scenario shards");
        manifest::Catalog prior{};
        manifest::LoadStatus priorStatus = manifest::LoadStatus::invalid;
        const bool priorReady =
            inventoried && !work->cacheCatalogPath.empty()
            && manifest::load(
                work->cacheCatalogPath.c_str(), work->sourceFingerprint, prior, priorStatus);
        if (inventoried && !topology::begin(work->activityInventory, work->canonicalTopology)) {
            failureDetail = "topology_begin_failed";
        }
        std::vector<manifest::Record> records;
        records.reserve(work->scenarios.size());
        work->scenarioWorldSources.clear();
        if (work->luaDeclarations) {
            work->scenarioWorldSources.reserve(work->scenarios.size());
        }
        complete = inventoried && containersReady && !work->scenarios.empty()
                   && work->canonicalTopology.scenarios.size() == work->scenarios.size();
        if (inventoried && work->scenarios.empty()) {
            failureDetail = "inventory_empty";
        } else if (!containersReady) {
            failureDetail = cancelled() ? "shard_cancelled" : "container_index_failed";
        }
        const ULONGLONG buildingStarted = GetTickCount64();
        std::vector<ScenarioBuildResult> scenarioResults;
        for (std::size_t first = 0; complete && first < work->scenarios.size();
             first += kScenarioBatchSize) {
            const std::size_t count =
                (std::min)(kScenarioBatchSize, work->scenarios.size() - first);
            if (!build_scenarios(
                    *work, source, containers, prior, priorReady, first, count, scenarioResults)) {
                complete = false;
                failureDetail = cancelled() ? "shard_cancelled" : "scenario_batch_failed";
                break;
            }
            for (std::size_t offset = 0; complete && offset < scenarioResults.size(); ++offset) {
                const Scenario& scenario = work->scenarios[first + offset];
                ScenarioBuildResult& result = scenarioResults[offset];
                if (!result.attempted || !result.ready || result.snapshot == nullptr) {
                    complete = false;
                    failureScenario = scenario.tag;
                    failureDetail =
                        result.detail[0] != '\0' ? result.detail.data() : "shard_build_failed";
                    break;
                }
                built += result.reused ? 0U : 1U;
                reused += result.reused ? 1U : 0U;
                if (!topology::append(*result.snapshot, work->canonicalTopology)) {
                    complete = false;
                    failureScenario = scenario.tag;
                    failureDetail = "topology_append_failed";
                    break;
                }
                if (!external_placements::append(
                        scenario.tag, *result.snapshot, work->externalPlacements)) {
                    complete = false;
                    failureScenario = scenario.tag;
                    failureDetail = "external_placements_failed";
                    break;
                }
                if (work->luaDeclarations) {
                    lua::ScenarioWorldSource worldSource{};
                    if (!build_world_source(*result.snapshot, worldSource)) {
                        complete = false;
                        failureScenario = scenario.tag;
                        failureDetail = "lua_world_source_failed";
                        break;
                    }
                    work->scenarioWorldSources.push_back(std::move(worldSource));
                }
                records.push_back(result.record);
            }
        }
        if (complete && !cancelled() && !topology::finish(work->canonicalTopology)) {
            complete = false;
            failureDetail = "topology_finish_failed";
        }
        if (complete && !cancelled()) {
            external_placements::finalize(work->externalPlacements);
            // A dropped harvest would silently shrink the squad anchors, so it fails the pass.
            if (!work->externalPlacements.complete) {
                complete = false;
                failureDetail = "external_placements_incomplete";
            }
        }
        if (complete && !cancelled()
            && !binding_ready(work->activityInventory.bindingCompleteness,
                              work->activityInventory.activities.size())) {
            complete = false;
            failureDetail = "binding_incomplete";
        }
        log_stage_duration("building", buildingStarted, work->scenarios.size(), complete);
        if (complete && !cancelled()) {
            // The pack stage reports no per-scenario counts, so the overlay bar is indeterminate.
            publish_progress(state::activity_sdk::generation::Status::building,
                             total,
                             total,
                             0,
                             "building generated SDK pack",
                             false);
            const ULONGLONG packStarted = GetTickCount64();
            complete = publish_estate(*work, source, records, failureDetail);
            log_stage_duration("pack", packStarted, work->scenarios.size(), complete);
            if (complete && !clean_stale_shards(work->scenarioDirectory, records)) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 "ev=activity_sdk_generation stage=shard_cleanup result=retained");
            }
        }
        if (cancelled()) {
            publish_progress(state::activity_sdk::generation::Status::cancelled,
                             static_cast<std::uint32_t>(records.size()),
                             static_cast<std::uint32_t>(work->scenarios.size()),
                             0,
                             "generation was cancelled");
        } else if (complete) {
            publish_progress(state::activity_sdk::generation::Status::ready,
                             static_cast<std::uint32_t>(records.size()),
                             static_cast<std::uint32_t>(records.size()),
                             0,
                             "generated SDK pack is ready");
        } else {
            publish_progress(state::activity_sdk::generation::Status::failed,
                             static_cast<std::uint32_t>(records.size()),
                             static_cast<std::uint32_t>(work->scenarios.size()),
                             failureScenario,
                             failureDetail);
        }
    } catch (...) {
        publish_progress(state::activity_sdk::generation::Status::failed,
                         0,
                         static_cast<std::uint32_t>(work->scenarios.size()),
                         0,
                         "unexpected generation exception");
        complete = false;
    }
    finish_offline(*work, complete, built, reused);
    if (!work->offline) {
        report_result({work->scenarios.size(),
                       work->activityRoots.size(),
                       work->activityVariants.size(),
                       built,
                       reused,
                       complete ? nullptr : failureDetail,
                       failureScenario,
                       complete});
    }
    SecureZeroMemory(&work->keys, sizeof work->keys);
    if (!work->offline) {
        core::ui::busy::end(core::ui::busy::Task::sdkGeneration);
    }
    return 0;
}

} // namespace

/** Adapts the selected live or offline cancellation source to shared stages. */
bool worker_internal::cancel_requested() noexcept {
    return cancelled();
}

/** Sends one worker event to the selected live or offline observer. */
void worker_internal::publish_progress(state::activity_sdk::generation::Status status,
                                       std::uint32_t current,
                                       std::uint32_t total,
                                       std::uint32_t scenarioTag,
                                       std::string_view detail,
                                       bool determinate) noexcept {
    if (g_offlineProgress != nullptr) {
        g_offlineProgress(g_offlineProgressContext, {status, current, total, scenarioTag, detail});
        return;
    }
    core::ui::busy::set_progress(
        core::ui::busy::Task::sdkGeneration, current, total, detail, determinate);
    state::activity_sdk::generation::internal::publish(status, current, total, scenarioTag, detail);
}

/** Runs one synchronous package pass into an isolated artifact tree. */
OfflineBuildStatus build_offline(const OfflineBuildRequest& request,
                                 OfflineCancelProbe cancel,
                                 void* cancelContext,
                                 OfflineProgressSink progress,
                                 void* progressContext,
                                 OfflineBuildResult& output) noexcept {
    output = {};
    const bool validFingerprint = std::any_of(request.sourceFingerprint.begin(),
                                              request.sourceFingerprint.end(),
                                              [](std::byte value) { return value != std::byte{}; });
    if (request.keys == nullptr) {
        return OfflineBuildStatus::invalidInput;
    }
    const auto keyBytes =
        std::span(reinterpret_cast<const std::byte*>(request.keys), sizeof *request.keys);
    const bool validKeys = std::any_of(
        keyBytes.begin(), keyBytes.end(), [](std::byte value) { return value != std::byte{}; });
    if (!validFingerprint || !validKeys || request.packageDirectory.empty()
        || request.outputArtifactDirectory.empty()
        || !isolated_output_ready(request.outputArtifactDirectory)) {
        return OfflineBuildStatus::invalidInput;
    }

    auto* work = new (std::nothrow) Work();
    if (work == nullptr) {
        return OfflineBuildStatus::failed;
    }
    OfflineBuildStatus status = OfflineBuildStatus::failed;
    try {
        work->keys = *request.keys;
        work->sourceFingerprint = request.sourceFingerprint;
        work->packageDirectory.assign(request.packageDirectory);
        const std::wstring outputRoot(request.outputArtifactDirectory);
        const std::wstring cacheRoot(request.cacheArtifactDirectory);
        work->sdkDirectory = outputRoot + std::wstring(kSdkDirectorySuffix);
        work->scenarioDirectory = outputRoot + std::wstring(kScenarioDirectorySuffix);
        work->catalogPath = outputRoot + std::wstring(kCatalogFileSuffix);
        work->packPath = outputRoot + std::wstring(kPackFileSuffix);
        if (!cacheRoot.empty()) {
            work->cacheScenarioDirectory = cacheRoot + std::wstring(kScenarioDirectorySuffix);
            work->cacheCatalogPath = cacheRoot + std::wstring(kCatalogFileSuffix);
        }
        work->offlineResult = &output;
        work->offlineStatus = &status;
        work->offline = true;
        // The isolated export publishes a complete estate, so it always writes the Lua tree.
        work->luaDeclarations = true;
    } catch (...) {
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        return OfflineBuildStatus::invalidInput;
    }

    const DWORD packageAttributes = GetFileAttributesW(work->packageDirectory.c_str());
    if (packageAttributes == INVALID_FILE_ATTRIBUTES
        || (packageAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || !ensure_directory(std::wstring(request.outputArtifactDirectory))
        || !ensure_directory(work->sdkDirectory) || !ensure_directory(work->scenarioDirectory)) {
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        return OfflineBuildStatus::invalidInput;
    }

    AcquireSRWLockExclusive(&g_offlineLock);
    AcquireSRWLockExclusive(&g_lock);
    const bool busy = g_offlineActive || g_enabled || g_thread != nullptr;
    if (!busy) {
        g_offlineActive = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (busy) {
        ReleaseSRWLockExclusive(&g_offlineLock);
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        return OfflineBuildStatus::busy;
    }

    g_cancel.store(false, std::memory_order_relaxed);
    g_offlineCancel = cancel;
    g_offlineCancelContext = cancelContext;
    g_offlineProgress = progress;
    g_offlineProgressContext = progressContext;
    (void)thread_main(work);
    g_offlineProgressContext = nullptr;
    g_offlineProgress = nullptr;
    g_offlineCancelContext = nullptr;
    g_offlineCancel = nullptr;
    g_cancel.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_lock);
    g_offlineActive = false;
    ReleaseSRWLockExclusive(&g_lock);
    ReleaseSRWLockExclusive(&g_offlineLock);
    return status;
}

/** Stores artifact paths without creating the optional SDK directory. */
void initialize(void* module, const Policy& policy) noexcept {
    core::path::Buffer artifact;
    const bool resolved = core::path::artifact_directory(module, artifact);
    std::wstring sdkDirectory;
    std::wstring scenarioDirectory;
    std::wstring catalogPath;
    std::wstring packPath;
    try {
        if (resolved) {
            sdkDirectory.assign(artifact.chars.data(), artifact.length);
            scenarioDirectory = sdkDirectory;
            catalogPath = sdkDirectory;
            packPath = sdkDirectory;
            sdkDirectory.append(kSdkDirectorySuffix);
            scenarioDirectory.append(kScenarioDirectorySuffix);
            catalogPath.append(kCatalogFileSuffix);
            packPath.append(kPackFileSuffix);
        }
    } catch (...) {
        sdkDirectory.clear();
        scenarioDirectory.clear();
        catalogPath.clear();
        packPath.clear();
    }
    AcquireSRWLockExclusive(&g_lock);
    g_configured = resolved && !sdkDirectory.empty() && !scenarioDirectory.empty()
                   && !catalogPath.empty() && !packPath.empty();
    g_enabled = g_configured;
    g_luaDeclarations = policy.luaDeclarations;
    g_started = false;
    g_packageReadyAfter = 0;
    g_module = module;
    g_sdkDirectory = std::move(sdkDirectory);
    g_scenarioDirectory = std::move(scenarioDirectory);
    g_catalogPath = std::move(catalogPath);
    g_packPath = std::move(packPath);
    g_cancel.store(false, std::memory_order_relaxed);
    const bool active = g_enabled;
    ReleaseSRWLockExclusive(&g_lock);
    publish_progress(active ? state::activity_sdk::generation::Status::waiting
                            : state::activity_sdk::generation::Status::disabled,
                     0,
                     0,
                     0,
                     active ? "waiting for package data" : "generation is disabled");
}

/** Starts the configured pass once every package-owned input is ready. */
void service() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_thread != nullptr && WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_configured || !g_enabled || g_started || g_offlineActive || g_thread != nullptr
        || now < g_packageReadyAfter) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (state::activity_sdk::status() == state::activity_sdk::Status::ready) {
        const state::activity_sdk::Snapshot loaded = state::activity_sdk::snapshot();
        generated::Digest fingerprint{};
        manifest::SdkIdentity identity{};
        const bool hasIdentity =
            loaded && loaded->sdk_build_sha256().size() == identity.buildSha256.size()
            && loaded->payload_sha256().size() == identity.payloadSha256.size()
            && state::content_manifest::visit_snapshot(&copy_fingerprint, &fingerprint);
        if (hasIdentity) {
            std::copy(loaded->sdk_build_sha256().begin(),
                      loaded->sdk_build_sha256().end(),
                      identity.buildSha256.begin());
            std::copy(loaded->payload_sha256().begin(),
                      loaded->payload_sha256().end(),
                      identity.payloadSha256.begin());
        }
        const bool current =
            hasIdentity
            && shard_store::compatible_catalog(
                g_catalogPath.c_str(), g_scenarioDirectory, fingerprint, identity)
            && (!g_luaDeclarations || lua::is_current(g_sdkDirectory.c_str(), *loaded));
        if (current) {
            g_started = true;
            publish_progress(state::activity_sdk::generation::Status::ready,
                             0,
                             0,
                             0,
                             "generated SDK estate is already loaded");
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        state::activity_sdk::shutdown();
    }
    auto* work = new (std::nothrow) Work();
    if (work == nullptr) {
        g_enabled = false;
        publish_progress(state::activity_sdk::generation::Status::failed,
                         0,
                         0,
                         0,
                         "SDK generator allocation failed");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    try {
        work->module = g_module;
        work->luaDeclarations = g_luaDeclarations;
        work->sdkDirectory = g_sdkDirectory;
        work->scenarioDirectory = g_scenarioDirectory;
        work->catalogPath = g_catalogPath;
        work->packPath = g_packPath;
    } catch (...) {
        delete work;
        g_enabled = false;
        publish_progress(
            state::activity_sdk::generation::Status::failed, 0, 0, 0, "SDK generator setup failed");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    core::path::Buffer packageDirectory;
    const bool ready = work->module != nullptr && !work->sdkDirectory.empty()
                       && !work->packPath.empty() && state::build_data::scenario_layouts_ready()
                       && items::packages::package_directory(packageDirectory)
                       && prepare_work(*work);
    if (ready) {
        try {
            work->packageDirectory.assign(packageDirectory.chars.data(), packageDirectory.length);
        } catch (...) {
            SecureZeroMemory(&work->keys, sizeof work->keys);
            delete work;
            g_enabled = false;
            publish_progress(state::activity_sdk::generation::Status::failed,
                             0,
                             0,
                             0,
                             "SDK package path setup failed");
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    if (!ready) {
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        publish_progress(
            state::activity_sdk::generation::Status::waiting, 0, 0, 0, "waiting for package data");
        g_packageReadyAfter = now + kPackageWaitMs;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (!ensure_directory(work->sdkDirectory) || !ensure_directory(work->scenarioDirectory)) {
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        g_enabled = false;
        publish_progress(state::activity_sdk::generation::Status::failed,
                         0,
                         0,
                         0,
                         "SDK output directory setup failed");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_cancel.store(false, std::memory_order_relaxed);
    publish_progress(
        state::activity_sdk::generation::Status::preparing,
        0,
        static_cast<std::uint32_t>(middleware::content::packages::tables::kActivityDefinitionCount),
        0,
        "waiting to build installed activity inventory");
    core::ui::busy::raise(core::ui::busy::Task::sdkGeneration);
    g_thread = CreateThread(nullptr, 0, &thread_main, work, 0, nullptr);
    if (g_thread == nullptr) {
        core::ui::busy::end(core::ui::busy::Task::sdkGeneration);
        SecureZeroMemory(&work->keys, sizeof work->keys);
        delete work;
        publish_progress(state::activity_sdk::generation::Status::failed,
                         0,
                         0,
                         0,
                         "SDK generator thread did not start");
        g_enabled = false;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_started = true;
    g_packageReadyAfter = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Loads only a shard named by the atomically published catalog. */
bool load_cached_scenario(std::uint32_t scenarioTag,
                          std::string_view scenarioName,
                          std::shared_ptr<catalog::Snapshot>& output) noexcept {
    output.reset();
    if (scenarioTag == 0 || scenarioName.empty()
        || scenarioName.size() >= catalog::kScenarioNameCapacity) {
        return false;
    }
    std::wstring scenarioDirectory;
    std::wstring catalogPath;
    AcquireSRWLockShared(&g_lock);
    const bool configured = g_configured;
    try {
        scenarioDirectory = g_scenarioDirectory;
        catalogPath = g_catalogPath;
    } catch (...) {
        scenarioDirectory.clear();
        catalogPath.clear();
    }
    ReleaseSRWLockShared(&g_lock);
    if (!configured || scenarioDirectory.empty() || catalogPath.empty()) {
        return false;
    }
    generated::Digest fingerprint{};
    if (!state::content_manifest::visit_snapshot(&copy_fingerprint, &fingerprint)) {
        return false;
    }
    try {
        manifest::Catalog catalog{};
        manifest::LoadStatus manifestStatus = manifest::LoadStatus::invalid;
        if (!manifest::load(catalogPath.c_str(), fingerprint, catalog, manifestStatus)
            || !final_sdk_identity(fingerprint, catalog.sdk)
            || !binding_ready(catalog.bindingCompleteness, catalog.activityVariants.size())) {
            return false;
        }
        const manifest::Record* const record = find_record(catalog, scenarioTag);
        if (record == nullptr || record->scenarioNameLength == 0
            || record->scenarioNameLength >= record->scenarioName.size()
            || !record_name_matches(*record, scenarioTag, scenarioName)) {
            return false;
        }
        Scenario scenario{};
        scenario.tag = scenarioTag;
        scenario.nameLength = record->scenarioNameLength;
        scenario.name = record->scenarioName;
        auto pending = std::make_shared<catalog::Snapshot>();
        if (!load_record(scenarioDirectory, fingerprint, scenario, *record, *pending)) {
            return false;
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output.reset();
        return false;
    }
}

/** Cancels generation before withdrawing all paths and policy. */
void reset() noexcept {
    HANDLE thread = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    g_enabled = false;
    g_cancel.store(true, std::memory_order_relaxed);
    thread = g_thread;
    ReleaseSRWLockExclusive(&g_lock);
    if (thread != nullptr) {
        WaitForSingleObject(thread, INFINITE);
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_thread != nullptr) {
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_configured = false;
    g_started = false;
    g_packageReadyAfter = 0;
    g_module = nullptr;
    g_sdkDirectory.clear();
    g_scenarioDirectory.clear();
    g_catalogPath.clear();
    g_packPath.clear();
    ReleaseSRWLockExclusive(&g_lock);
    core::ui::busy::end(core::ui::busy::Task::sdkGeneration);
    state::activity_sdk::generation::internal::clear();
}

} // namespace sunrise::client::content::activity::sdk_generation
