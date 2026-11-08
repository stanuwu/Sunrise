#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../state/activity_sdk/generated_world/codec.h"
#include "../../../state/activity_sdk/generation/definition.h"
#include "../../../state/build_data/runtime.h"
#include "activity_sdk_generation_worker_internal.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {
namespace {

/** Parallel readers divide one pass-wide cache budget instead of multiplying it per core. */
constexpr std::size_t kParallelBlockCacheBudget = 256;
constexpr std::size_t kParallelTableCacheBudget = 16;
constexpr std::size_t kMaximumScenarioWorkers = 12;
constexpr std::size_t kScenarioChunkSize = 1;

/** Counts the rows behind a refused shard, which the coverage verdict alone cannot name. */
void report_shard_diagnostics(const Scenario& scenario,
                              const catalog::Snapshot& snapshot) noexcept {
    std::size_t incompleteObjects = 0;
    std::size_t incompleteSafety = 0;
    std::size_t leaves = 0;
    std::size_t bareTargets = 0;
    for (const catalog::Object& object : snapshot.objects) {
        if (!object.complete) {
            ++incompleteObjects;
        }
        if (object.safety == catalog::GroupSafety::incomplete) {
            ++incompleteSafety;
        }
        leaves += object.placedLeafCount;
        bareTargets += object.bareTargetCount;
    }
    std::size_t unresolvedStates = 0;
    for (const catalog::State& state : snapshot.states) {
        if (!state.resolved) {
            ++unresolvedStates;
        }
    }
    const catalog::ContainerPlacementDiagnostics& placements =
        snapshot.containerPlacementDiagnostics;
    std::array<char, 1024> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=sdk_shard_diag scenario=0x%08X objects=%zu "
                      "obj_incomplete=%zu obj_safety_incomplete=%zu leaves=%zu "
                      "bare=%zu states_unresolved=%zu "
                      "unresolved_reads=%llu spatial_ctx=%u spatial_na=%u spatial_complete=%u "
                      "spatial_unresolved=%llu spatial_dropped=%llu spatial_semantic=%llu "
                      "spatial_tables=%zu spatial_owners=%zu spatial_instances=%zu "
                      "cp_ctx=%u cp_na=%u cp_complete=%u cp_owner_complete=%u "
                      "cp_unresolved=%llu cp_semantic=%llu cp_dropped=%llu/%llu/%llu/%llu/%llu "
                      "cp_rows=%zu/%zu/%zu/%zu/%zu",
                      static_cast<unsigned>(scenario.tag),
                      snapshot.objects.size(),
                      incompleteObjects,
                      incompleteSafety,
                      leaves,
                      bareTargets,
                      unresolvedStates,
                      static_cast<unsigned long long>(snapshot.unresolvedReads),
                      snapshot.staticSpatialContextResolved ? 1U : 0U,
                      snapshot.staticSpatialNotApplicable ? 1U : 0U,
                      snapshot.staticSpatialComplete ? 1U : 0U,
                      static_cast<unsigned long long>(snapshot.staticSpatialUnresolvedReads),
                      static_cast<unsigned long long>(snapshot.staticSpatialDropped),
                      static_cast<unsigned long long>(snapshot.staticSpatialSemanticUnresolved),
                      snapshot.staticSpatialTables.size(),
                      snapshot.staticSpatialOwners.size(),
                      snapshot.staticSpatialInstances.size(),
                      placements.contextResolved ? 1U : 0U,
                      placements.contextNotApplicable ? 1U : 0U,
                      placements.complete ? 1U : 0U,
                      placements.identityOwnerInventoryComplete ? 1U : 0U,
                      static_cast<unsigned long long>(placements.unresolvedReads),
                      static_cast<unsigned long long>(placements.semanticUnresolved),
                      static_cast<unsigned long long>(placements.droppedLists),
                      static_cast<unsigned long long>(placements.droppedOwners),
                      static_cast<unsigned long long>(placements.droppedPlacements),
                      static_cast<unsigned long long>(placements.droppedConfigs),
                      static_cast<unsigned long long>(placements.droppedComponents),
                      snapshot.containerPlacementLists.size(),
                      snapshot.containerPlacementOwners.size(),
                      snapshot.containerPlacements.size(),
                      snapshot.containerPlacementConfigs.size(),
                      snapshot.containerPlacementComponents.size());
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::error,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }
}

/** Writes one new shard under its deterministic digest name. */
[[nodiscard]] bool build_record(const Work& work,
                                const package_reader::Source& source,
                                const builder::ContainerIndex& containers,
                                package_reader::Scratch& scratch,
                                builder::ScenarioAnalysisCache& analyses,
                                const Scenario& scenario,
                                manifest::Record& record,
                                std::shared_ptr<const catalog::Snapshot>& output,
                                std::array<char, kDetailCapacity>& detailScratch,
                                const char*& detail) {
    // Each failure arm reports its own detail token.
    const std::string_view name(scenario.name.data(), scenario.nameLength);
    const auto snapshot = builder::build_scenario_catalog(
        source, containers, scratch, analyses, work.tables, scenario.tag, name, &cancel_requested);
    if (cancel_requested()) {
        detail = "shard_cancelled";
        return false;
    }
    if (!snapshot) {
        detail = "shard_catalog_null";
        return false;
    }
    if (snapshot->status != catalog::BuildStatus::ready) {
        const char* stage = "shard_catalog_not_ready";
        switch (snapshot->status) {
        case catalog::BuildStatus::failed:
            stage = "shard_catalog_failed";
            break;
        case catalog::BuildStatus::idle:
            stage = "shard_catalog_idle";
            break;
        case catalog::BuildStatus::queued:
            stage = "shard_catalog_queued";
            break;
        case catalog::BuildStatus::building:
            stage = "shard_catalog_building";
            break;
        default:
            break;
        }
        report_shard_diagnostics(scenario, *snapshot);
        // The builder already names its own refusal. Carry that text out instead of dropping it.
        const char* reason = snapshot->detail[0] != '\0' ? snapshot->detail.data() : "no detail";
        detailScratch = {};
        (void)std::snprintf(detailScratch.data(), detailScratch.size(), "%s:%s", stage, reason);
        detail = detailScratch.data();
        return false;
    }
    if (!snapshot->containerPlacementDiagnostics.identityOwnerInventoryComplete) {
        detail = "shard_container_inventory_incomplete";
        report_shard_diagnostics(scenario, *snapshot);
        return false;
    }
    generated_world::PreparedShard prepared{};
    if (!generated_world::prepare(work.sourceFingerprint, *snapshot, prepared)) {
        detail = "shard_prepare_failed";
        return false;
    }
    const generated_world::Digest payload = prepared.payload_sha256();
    std::wstring finalPath;
    generated_world::Digest written{};
    if (!shard_path(work.scenarioDirectory, scenario.tag, payload, finalPath)) {
        detail = "shard_path_failed";
        return false;
    }
    if (!generated_world::publish(finalPath.c_str(), std::move(prepared), written)) {
        detail = "shard_write_failed";
        return false;
    }
    record = {};
    record.scenarioTag = scenario.tag;
    record.scenarioName = scenario.name;
    record.scenarioNameLength = scenario.nameLength;
    record.shardPayloadSha256 = payload;
    output = snapshot;
    return true;
}

/** Rewrites one loaded cache hit into an isolated output tree. */
[[nodiscard]] bool
materialize_cached_record(const Work& work,
                          const Scenario& scenario,
                          const manifest::Record& record,
                          const std::shared_ptr<const catalog::Snapshot>& cached,
                          std::shared_ptr<const catalog::Snapshot>& output) noexcept {
    if (cached == nullptr) {
        return false;
    }
    if (work.cacheScenarioDirectory == work.scenarioDirectory) {
        output = cached;
        return true;
    }
    std::wstring finalPath;
    generated_world::Digest written{};
    if (!shard_path(work.scenarioDirectory, scenario.tag, record.shardPayloadSha256, finalPath)
        || !generated_world::write(finalPath.c_str(), work.sourceFingerprint, *cached, written)) {
        return false;
    }
    output = cached;
    return true;
}

/** Shared immutable inputs and atomic scheduling state for one scenario batch. */
struct ScenarioBuildBatch final {
    const Work* work{};
    const package_reader::Source* source{};
    const builder::ContainerIndex* containers{};
    const manifest::Catalog* prior{};
    std::vector<ScenarioBuildResult>* results{};
    std::atomic_size_t next{};
    std::atomic_size_t completed{};
    SRWLOCK progressLock = SRWLOCK_INIT;
    std::size_t progressPublished{};
    std::size_t firstScenario{};
    std::size_t blockCacheSlots{};
    std::size_t tableCacheSlots{};
    bool priorReady{};
};

/** Caps workers while leaving cores for the game and the server. */
[[nodiscard]] std::size_t scenario_worker_count(std::size_t scenarios) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::size_t processors = static_cast<std::size_t>(info.dwNumberOfProcessors);
    const std::size_t available = processors > 2U ? processors - 2U : 1U;
    return (std::min)(scenarios, (std::min)(available, kMaximumScenarioWorkers));
}

/** Divides one cache budget across active workers without leaving a worker uncached. */
[[nodiscard]] std::size_t worker_cache_slots(std::size_t budget, std::size_t workers) noexcept {
    return (std::max)(std::size_t{1}, (budget + workers - 1U) / workers);
}

/** Publishes only increasing completion counts from out-of-order workers. */
void publish_parallel_progress(ScenarioBuildBatch& batch, const Scenario& scenario) noexcept {
    const std::size_t complete = batch.completed.fetch_add(1U) + 1U;
    AcquireSRWLockExclusive(&batch.progressLock);
    if (complete <= batch.progressPublished) {
        ReleaseSRWLockExclusive(&batch.progressLock);
        return;
    }
    batch.progressPublished = complete;
    publish_progress(state::activity_sdk::generation::Status::building,
                     static_cast<std::uint32_t>(complete),
                     static_cast<std::uint32_t>(batch.work->scenarios.size()),
                     scenario.tag,
                     std::string_view(scenario.name.data(), scenario.nameLength));
    ReleaseSRWLockExclusive(&batch.progressLock);
}

/** A cached shard of a laid-out scenario is reusable only when its placement contexts resolved. */
[[nodiscard]] bool placement_context_ready(const Work& work,
                                           const Scenario& scenario,
                                           const catalog::Snapshot& snapshot) noexcept {
    state::build_data::scenarios::Definition layout{};
    const std::string_view name(scenario.name.data(), scenario.nameLength);
    if (!::sunrise::client::content::scenarios::find_layout(work.tables.layouts, name, layout)
        || layout.spawnStemLength == 0) {
        return true;
    }
    return snapshot.containerPlacementDiagnostics.contextResolved
           && snapshot.staticSpatialContextResolved;
}

/** Builds one worker's contiguous chunks with private package and analysis caches. */
void run_scenario_worker(ScenarioBuildBatch& batch) noexcept {
    std::unique_ptr<package_reader::Scratch> scratch(new (std::nothrow) package_reader::Scratch());
    if (scratch == nullptr) {
        return;
    }
    if (!package_reader::prepare_blocks(*scratch, batch.blockCacheSlots)) {
        (void)package_reader::prepare_blocks(*scratch, 0);
    }
    if (!package_reader::prepare_tables(*scratch, batch.tableCacheSlots)) {
        (void)package_reader::prepare_tables(*scratch, 0);
    }
    builder::ScenarioAnalysisCache analyses{};
    while (!cancel_requested()) {
        const std::size_t begin = batch.next.fetch_add(kScenarioChunkSize);
        if (begin >= batch.results->size()) {
            break;
        }
        const std::size_t end = (std::min)(begin + kScenarioChunkSize, batch.results->size());
        for (std::size_t index = begin; index < end && !cancel_requested(); ++index) {
            const Scenario& scenario = batch.work->scenarios[batch.firstScenario + index];
            ScenarioBuildResult& result = (*batch.results)[index];
            result.attempted = true;
            try {
                const manifest::Record* existing =
                    batch.priorReady ? find_record(*batch.prior, scenario.tag) : nullptr;
                bool kept = existing != nullptr
                            && load_full_record(batch.work->cacheScenarioDirectory,
                                                batch.work->sourceFingerprint,
                                                scenario,
                                                *existing,
                                                result.snapshot);
                if (kept && !placement_context_ready(*batch.work, scenario, *result.snapshot)) {
                    kept = false;
                }
                if (kept
                    && !materialize_cached_record(
                        *batch.work, scenario, *existing, result.snapshot, result.snapshot)) {
                    kept = false;
                }
                if (kept) {
                    result.record = *existing;
                    result.reused = true;
                    result.ready = true;
                } else {
                    const char* detail = "shard_build_failed";
                    std::array<char, kDetailCapacity> scratchDetail{};
                    result.ready = build_record(*batch.work,
                                                *batch.source,
                                                *batch.containers,
                                                *scratch,
                                                analyses,
                                                scenario,
                                                result.record,
                                                result.snapshot,
                                                scratchDetail,
                                                detail);
                    if (!result.ready) {
                        (void)std::snprintf(
                            result.detail.data(), result.detail.size(), "%s", detail);
                    }
                }
            } catch (...) {
                result.ready = false;
                (void)std::snprintf(result.detail.data(),
                                    result.detail.size(),
                                    "%s",
                                    "unexpected scenario build exception");
            }
            publish_parallel_progress(batch, scenario);
        }
    }
    package_reader::close_files(*scratch);
}

/** Adapts one batch worker to the Windows thread ABI. */
DWORD WINAPI scenario_thread_main(void* opaque) noexcept {
    run_scenario_worker(*static_cast<ScenarioBuildBatch*>(opaque));
    return 0;
}

} // namespace

/** Builds all scenario snapshots in parallel while retaining scenario-order output. */
bool build_scenarios(Work& work,
                     const package_reader::Source& source,
                     const builder::ContainerIndex& containers,
                     const manifest::Catalog& prior,
                     bool priorReady,
                     std::size_t firstScenario,
                     std::size_t scenarioCount,
                     std::vector<ScenarioBuildResult>& results) {
    results.clear();
    results.resize(scenarioCount);
    ScenarioBuildBatch batch{};
    batch.work = &work;
    batch.source = &source;
    batch.containers = &containers;
    batch.prior = &prior;
    batch.results = &results;
    batch.completed.store(firstScenario);
    batch.progressPublished = firstScenario;
    batch.firstScenario = firstScenario;
    batch.priorReady = priorReady;
    const std::size_t workers = scenario_worker_count(results.size());
    if (workers == 0) {
        return false;
    }
    batch.blockCacheSlots = worker_cache_slots(kParallelBlockCacheBudget, workers);
    batch.tableCacheSlots = worker_cache_slots(kParallelTableCacheBudget, workers);
    std::vector<HANDLE> threads;
    threads.reserve(workers - 1U);
    for (std::size_t index = 1; index < workers; ++index) {
        const HANDLE thread = CreateThread(nullptr, 0, &scenario_thread_main, &batch, 0, nullptr);
        if (thread != nullptr) {
            threads.push_back(thread);
        }
    }
    run_scenario_worker(batch);
    for (const HANDLE thread : threads) {
        (void)WaitForSingleObject(thread, INFINITE);
        (void)CloseHandle(thread);
    }
    return !cancel_requested();
}

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
