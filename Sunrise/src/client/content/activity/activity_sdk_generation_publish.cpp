#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../state/activity_sdk/internal.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/gameplay/external/rsat_decode_plans.h"
#include "activity_sdk_decode_plans.h"
#include "activity_sdk_decode_plans_image.h"
#include "activity_sdk_generation_worker_internal.h"
#include "activity_sdk_live_publication.h"
#include "activity_sdk_native_pack_pipeline.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {
namespace {

namespace live = live_publication;
namespace native_pack = native_pack_pipeline;

/** Runtime reload inputs stay alive until the file-scoped transaction finalizer returns. */
struct ReloadContext final {
    void* module{};
    state::activity_sdk::ExpectedIdentity expected{};
    state::activity_sdk::ExpectedIdentity prior{};
    bool hasPrior{};
    bool installedNew{};
};

struct PackProgressContext final {
    bool live{};
    std::uint32_t total{};
    ULONGLONG phaseStarted{};
    native_pack::Phase phase{native_pack::Phase::activityMetadata};
    bool phaseActive{};
};

/** @return The progress-line detail text for one native pack phase. */
[[nodiscard]] std::string_view phase_detail(native_pack::Phase phase) noexcept {
    switch (phase) {
    case native_pack::Phase::activityMetadata:
        return "collecting activity metadata";
    case native_pack::Phase::worldTopology:
        return "building world topology";
    case native_pack::Phase::squadFacts:
        return "reading squad and object facts";
    case native_pack::Phase::actorDefinitions:
        return "reading actor definitions";
    case native_pack::Phase::squadLinks:
        return "linking squads to world objects";
    case native_pack::Phase::authoredSceneFacts:
        return "reading authored scene facts";
    case native_pack::Phase::authoredSceneLinks:
        return "building authored scene links";
    case native_pack::Phase::dialogueCues:
        return "reading authored dialogue cues";
    case native_pack::Phase::authoredText:
        return "reading authored dialogue and HUD text";
    case native_pack::Phase::behaviors:
        return "reading compiled behavior roots";
    case native_pack::Phase::actionPolicies:
        return "building action policies";
    case native_pack::Phase::packTables:
        return "composing SDK tables";
    case native_pack::Phase::luaDeclarations:
        return "generating Lua declarations";
    case native_pack::Phase::outputFiles:
        return "writing generated SDK files";
    }
    return "building generated SDK pack";
}

/** @return The stable log name of one native pack phase. */
[[nodiscard]] std::string_view phase_name(native_pack::Phase phase) noexcept {
    switch (phase) {
    case native_pack::Phase::activityMetadata:
        return "activity_metadata";
    case native_pack::Phase::worldTopology:
        return "world_topology";
    case native_pack::Phase::squadFacts:
        return "squad_facts";
    case native_pack::Phase::actorDefinitions:
        return "actor_definitions";
    case native_pack::Phase::squadLinks:
        return "squad_links";
    case native_pack::Phase::authoredSceneFacts:
        return "authored_scene_facts";
    case native_pack::Phase::authoredSceneLinks:
        return "authored_scene_links";
    case native_pack::Phase::dialogueCues:
        return "dialogue_cues";
    case native_pack::Phase::authoredText:
        return "authored_text";
    case native_pack::Phase::behaviors:
        return "behaviors";
    case native_pack::Phase::actionPolicies:
        return "action_policies";
    case native_pack::Phase::packTables:
        return "pack_tables";
    case native_pack::Phase::luaDeclarations:
        return "lua_declarations";
    case native_pack::Phase::outputFiles:
        return "output_files";
    }
    return "unknown";
}

/** Logs one finished pack phase and how long it took. */
void log_pack_phase(native_pack::Phase phase, ULONGLONG started) noexcept {
    std::array<char, 128> line{};
    const std::string_view name = phase_name(phase);
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_sdk_pack phase=%.*s ms=%llu",
                                      static_cast<int>(name.size()),
                                      name.data(),
                                      static_cast<unsigned long long>(GetTickCount64() - started));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Names the check a refused staged estate failed, because the status alone cannot. */
void log_catalog_refusal(const char* status) noexcept {
    std::array<char, 224> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_sdk_catalog result=%s check=%s",
                                      status,
                                      state::activity_sdk::last_catalog_reason());
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::error,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

void finish_pack_progress(PackProgressContext& progress) noexcept {
    if (progress.live && progress.phaseActive) {
        log_pack_phase(progress.phase, progress.phaseStarted);
        progress.phaseActive = false;
    }
}

/** Progress callback for the native pack: closes the previous phase and opens this one. */
void report_pack_progress(void* context, native_pack::Phase phase) noexcept {
    if (context == nullptr) {
        return;
    }
    auto& progress = *static_cast<PackProgressContext*>(context);
    if (progress.live) {
        if (progress.phaseActive) {
            log_pack_phase(progress.phase, progress.phaseStarted);
        }
        progress.phase = phase;
        progress.phaseStarted = GetTickCount64();
        progress.phaseActive = true;
        core::ui::busy::set_progress(core::ui::busy::Task::sdkGeneration,
                                     progress.total,
                                     progress.total,
                                     phase_detail(phase),
                                     false);
    }
}

void report_publication_progress(const PackProgressContext& progress,
                                 std::string_view detail) noexcept {
    if (progress.live) {
        core::ui::busy::set_progress(
            core::ui::busy::Task::sdkGeneration, progress.total, progress.total, detail, false);
    }
}

/** Copies one mapped catalog identity for a possible file-transaction rollback. */
[[nodiscard]] bool snapshot_identity(const state::activity_sdk::Snapshot& snapshot,
                                     state::activity_sdk::ExpectedIdentity& output) noexcept {
    output = {};
    if (!snapshot || snapshot->sdk_build_sha256().size() != output.sdkBuildSha256.size()
        || snapshot->payload_sha256().size() != output.payloadSha256.size()
        || snapshot->content_key_sha256().size() != output.contentKeySha256.size()
        || snapshot->logical_ir_sha256().size() != output.logicalIrSha256.size()) {
        return false;
    }
    std::copy(snapshot->sdk_build_sha256().begin(),
              snapshot->sdk_build_sha256().end(),
              output.sdkBuildSha256.begin());
    std::copy(snapshot->payload_sha256().begin(),
              snapshot->payload_sha256().end(),
              output.payloadSha256.begin());
    std::copy(snapshot->content_key_sha256().begin(),
              snapshot->content_key_sha256().end(),
              output.contentKeySha256.begin());
    std::copy(snapshot->logical_ir_sha256().begin(),
              snapshot->logical_ir_sha256().end(),
              output.logicalIrSha256.begin());
    return true;
}

/** Reloads the new pack first and the prior runtime identity after a file rollback. */
[[nodiscard]] bool reload_runtime(void* context) noexcept {
    if (context == nullptr) {
        return false;
    }
    auto& reload = *static_cast<ReloadContext*>(context);
    if (reload.module == nullptr) {
        return false;
    }
    if (!reload.installedNew) {
        if (!state::activity_sdk::reload(reload.module, reload.expected)) {
            return false;
        }
        reload.installedNew = true;
        return true;
    }
    if (!reload.hasPrior) {
        state::activity_sdk::shutdown();
        reload.installedNew = false;
        return true;
    }
    if (!state::activity_sdk::reload(reload.module, reload.prior)) {
        return false;
    }
    reload.installedNew = false;
    return true;
}

[[nodiscard]] bool native_cancel(void*) noexcept;

/** Logs one finished decoder cache build with its counts. */
void log_decoder_cache(decode_plans::Status status,
                       const decode_plans::Result& result,
                       ULONGLONG started) noexcept {
    std::array<char, 224> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_sdk_decoder_cache result=%s slots=%u pairs=%u plans=%u "
                      "active=%u errors=%u entries=%u schemas=%u fields=%u ms=%llu",
                      decode_plans::status_name(status),
                      result.slots,
                      result.pairs,
                      result.plans,
                      result.active,
                      result.errors,
                      result.entries,
                      result.schemas,
                      result.fields,
                      static_cast<unsigned long long>(GetTickCount64() - started));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            status == decode_plans::Status::ready ? core::log::Level::info
                                                  : core::log::Level::error,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Keeps the decoder pair beside the pack whose identity it uses. */
[[nodiscard]] bool build_decoder_cache(const decode_plans::Image& image,
                                       std::wstring_view packPath,
                                       const package_reader::Source& source,
                                       const state::activity_sdk::Catalog& catalog,
                                       const char*& failureDetail) noexcept {
    const ULONGLONG started = GetTickCount64();
    decode_plans::Result result{};
    decode_plans::Status status = decode_plans::Status::invalidInput;
    try {
        const std::size_t separator = packPath.find_last_of(L"\\/");
        const std::wstring cachePath =
            (separator == std::wstring_view::npos ? std::wstring{}
                                                  : std::wstring(packPath.substr(0, separator)))
            + std::wstring(state::gameplay::rsat_decode_plans::kCacheSuffix);
        decode_plans::Request request{};
        request.image = &image;
        request.source = &source;
        request.descriptors = catalog.sobject_rsat_descriptors();
        request.sdkBuildSha256 = catalog.sdk_build_sha256();
        request.cachePath = cachePath;
        request.cancel = &native_cancel;
        status = decode_plans::build(request, result);
    } catch (...) {
        status = decode_plans::Status::allocation;
    }
    log_decoder_cache(status, result, started);
    if (status != decode_plans::Status::ready) {
        failureDetail = "decoder_cache_failed";
        return false;
    }
    return true;
}

/** Adapts the selected live or offline worker cancellation source to native pack stages. */
[[nodiscard]] bool native_cancel(void*) noexcept {
    return cancel_requested();
}

} // namespace

/** Publishes one complete estate. @param failureDetail Receives the refusing step's name. */
bool publish_estate(Work& work,
                    const package_reader::Source& source,
                    std::span<const manifest::Record> records,
                    const char*& failureDetail) noexcept {
    failureDetail = "estate_publication_failed";
    if (records.empty() || cancel_requested()
        || !binding_ready(work.activityInventory.bindingCompleteness,
                          work.activityInventory.activities.size())) {
        failureDetail = cancel_requested() ? "cancelled_before_publication" : "binding_incomplete";
        return false;
    }

    // One reflection image serves the actor runtime schemas and the decoder cache.
    const auto imageStarted = GetTickCount64();
    decode_plans::Image image;
    const auto imageStatus = image.open(work.executablePath.c_str(), work.executableModule);
    if (imageStatus != decode_plans::Status::ready) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::error,
                          "ev=activity_sdk_image result=failed reason=%s",
                          decode_plans::status_name(imageStatus));
        failureDetail = "executable_image_failed";
        return false;
    }
    core::log::write_elapsed(
        core::log::Channel::client, "ev=activity_sdk_image", imageStarted, "ok");

    live::Stage stage{};
    PackProgressContext progress{
        !work.offline,
        static_cast<std::uint32_t>(
            (std::min)(work.scenarios.size(),
                       static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())))};
    const wchar_t* sdkDirectory = work.sdkDirectory.c_str();
    const wchar_t* packPath = work.packPath.c_str();
    const wchar_t* catalogPath = work.catalogPath.c_str();
    if (!work.offline) {
        report_publication_progress(progress, "preparing isolated SDK output");
        const live::Status allocated = live::allocate(work.packPath.c_str(), stage);
        if (allocated != live::Status::ready) {
            failureDetail = live::status_name(allocated);
            return false;
        }
        sdkDirectory = stage.sdkDirectory.c_str();
        packPath = stage.packPath.c_str();
        catalogPath = stage.catalogPath.c_str();
    }

    native_pack::Result packResult{};
    const native_pack::Status packStatus = native_pack::stage(sdkDirectory,
                                                              packPath,
                                                              source,
                                                              image,
                                                              work.sourceFingerprint,
                                                              work.activityInventory,
                                                              work.canonicalTopology,
                                                              work.scenarioWorldSources,
                                                              work.externalPlacements,
                                                              work.luaDeclarations,
                                                              &native_cancel,
                                                              nullptr,
                                                              &report_pack_progress,
                                                              &progress,
                                                              packResult);
    finish_pack_progress(progress);
    const manifest::SdkIdentity sdkIdentity{packResult.identity.sdkBuildSha256,
                                            packResult.identity.payloadSha256};
    bool complete = packStatus == native_pack::Status::ready && !cancel_requested();
    if (!complete) {
        failureDetail =
            cancel_requested() ? "cancelled_during_staging" : native_pack::status_name(packStatus);
    } else {
        work.payloadSha256 = packResult.payloadSha256;
        work.packBytes = packResult.fileBytes;
        work.luaFiles = packResult.luaFiles;
    }
    std::shared_ptr<state::activity_sdk::Catalog> stagedCatalog{};
    state::activity_sdk::Status stagedCatalogStatus = state::activity_sdk::Status::catalogInvalid;
    if (complete
        && !state::activity_sdk::load_path_expected(
            packPath, packResult.identity, stagedCatalog, stagedCatalogStatus)) {
        complete = false;
        failureDetail = state::activity_sdk::status_name(stagedCatalogStatus);
        log_catalog_refusal(failureDetail);
    }
    if (complete) {
        report_publication_progress(progress, "building decoder cache");
        complete = build_decoder_cache(image, packPath, source, *stagedCatalog, failureDetail);
    }
    stagedCatalog.reset();
    if (complete) {
        report_publication_progress(progress, "writing generated SDK catalog");
    }
    if (complete
        && !manifest::write(catalogPath,
                            work.sourceFingerprint,
                            sdkIdentity,
                            records,
                            work.activityRoots,
                            work.activityVariants)) {
        complete = false;
        failureDetail = "catalog_staging_failed";
    }
    if (complete && !work.offline) {
        report_publication_progress(progress, "publishing generated SDK estate");
        ReloadContext reload{work.module, packResult.identity};
        reload.hasPrior = snapshot_identity(state::activity_sdk::snapshot(), reload.prior);
        const live::Status published = live::publish(stage,
                                                     work.sdkDirectory.c_str(),
                                                     work.packPath.c_str(),
                                                     work.catalogPath.c_str(),
                                                     nullptr,
                                                     nullptr,
                                                     &reload_runtime,
                                                     &reload);
        complete = published == live::Status::ready;
        if (!complete) {
            failureDetail = live::status_name(published);
        }
    }
    if (!work.offline) {
        (void)live::discard(stage);
    }
    return complete;
}

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
