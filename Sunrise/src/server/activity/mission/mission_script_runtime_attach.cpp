/**
 * Resolving, opening and starting one mission program for a Host slot.
 * Every function here runs under the mission runtime lock its caller already holds.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/crypto/sha256.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../bap/runtime.h"
#include "../activity_sdk_mission_runtime.h"
#include "../host_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_sdk_bridge.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

enum class SourceStatus : std::uint8_t {
    ready,
    missing,
    fileError,
    tooLarge,
};

/** One explicit reload may replace the source hash for its exact binding. */
struct ReloadAuthorization final {
    state::activity::SessionBinding binding{};
    mission_state::ProgramKey program{};
    bool occupied{};
};

std::array<ReloadAuthorization, host::kInstanceCapacity> g_reloadAuthorizations{};
std::array<char, lua_vm::kSourceByteCapacity> g_source{};
core::path::Buffer g_scriptRoot{};
std::array<char, 1024> g_sdkLuaSearchPath{};

[[nodiscard]] ReloadAuthorization*
reload_authorization(const state::activity::SessionBinding& binding) noexcept {
    for (ReloadAuthorization& authorization : g_reloadAuthorizations) {
        if (authorization.occupied && same_binding(authorization.binding, binding)) {
            return &authorization;
        }
    }
    return nullptr;
}

[[nodiscard]] ReloadAuthorization* free_reload_authorization() noexcept {
    for (ReloadAuthorization& authorization : g_reloadAuthorizations) {
        if (!authorization.occupied) {
            return &authorization;
        }
    }
    return nullptr;
}

/** Handle of the module holding this code, found from a data address inside it. */
[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(g_source.data());
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           address,
                           &module)
        == FALSE) {
        return nullptr;
    }
    return module;
}

/** True when the link, the SDK view and the world view still match what the instance bound. */
[[nodiscard]] bool still_exact(RuntimeInstance& instance) noexcept {
    server::bap::ActivityLinkView link{};
    lua_vm::WorldGenerationIdentity worldGeneration{};
    return server::bap::activity_link_view(instance.view.binding, link)
           && link.publicTarget == instance.publicTarget
           && sdk::revalidate(instance.view,
                              instance.view.binding,
                              link.matchingLinks,
                              link.activityClientGeneration)
                  == sdk::Status::ready
           && instance.worldView.activity_sdk_view().catalog == instance.view.catalog
           && instance.worldView.activity_sdk_view().activityRow == instance.view.activityRow
           && instance.worldView.activity_sdk_view().activityClientGeneration
                  == instance.view.activityClientGeneration
           && sdk_bridge::world_generation_identity(instance.worldView, worldGeneration);
}

/**
 * Re-points one open program at the current ActivityClient generation.
 * @return True when the instance holds an exact view of the same program.
 */
[[nodiscard]] bool rebind_instance(RuntimeInstance& instance) noexcept {
    server::bap::ActivityLinkView link{};
    if (!server::bap::activity_link_view(instance.view.binding, link) || !link.joined) {
        return false;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    // A different SDK build is a different module, so that program opens again.
    if (catalog == nullptr || catalog != instance.view.catalog) {
        return false;
    }
    sdk::BoundView view{};
    const sdk::Selection selection{
        .binding = instance.view.binding,
        .matchingLinks = link.matchingLinks,
        .activityClientGeneration = link.activityClientGeneration,
    };
    if (sdk::resolve(catalog, selection, view) != sdk::Status::ready
        || view.activityRow != instance.view.activityRow) {
        return false;
    }
    generated::GeneratedWorldView worldView{};
    if (generated::resolve(view, worldView) != generated::BindStatus::ready) {
        return false;
    }
    instance.view = std::move(view);
    instance.worldView = std::move(worldView);
    instance.publicTarget = link.publicTarget;
    instance.playerKey = link.playerKey;
    instance.identity.playerKey = link.playerKey;
    instance.identity.publicTarget = link.publicTarget;
    // The bridge copies the world generation, so hand the program the rebuilt pair.
    return lua_vm::rebind(instance.vm,
                          instance.identity,
                          sdk_bridge::definition_api(instance.view, instance.worldView));
}

/** Folds the activity name into a lowercase file stem; other bytes become single underscores. */
[[nodiscard]] bool controller_stem(const sdk::Catalog& catalog,
                                   const format::Activity& activity,
                                   std::span<char> output) noexcept {
    if (output.size() < 2) {
        return false;
    }
    const std::string_view internal = catalog.string(activity.internalName);
    const std::string_view name =
        internal.empty() ? catalog.string(activity.displayName) : internal;
    std::size_t length = 0;
    bool separator = false;
    for (const unsigned char byte : name) {
        const bool digit = byte >= '0' && byte <= '9';
        const bool upper = byte >= 'A' && byte <= 'Z';
        const bool lower = byte >= 'a' && byte <= 'z';
        if (digit || upper || lower) {
            if (separator && length != 0) {
                if (length + 1 >= output.size()) {
                    return false;
                }
                output[length++] = '_';
            }
            separator = false;
            if (length + 1 >= output.size()) {
                return false;
            }
            output[length++] = static_cast<char>(upper ? byte + ('a' - 'A') : byte);
        } else {
            separator = true;
        }
    }
    if (length == 0) {
        // A slot with no authored name reports this fixed spelling.
        constexpr std::string_view unnamed = "unnamed";
        if (unnamed.size() + 1 > output.size()) {
            return false;
        }
        std::copy(unnamed.begin(), unnamed.end(), output.begin());
        length = unnamed.size();
    }
    if (output[0] >= '0' && output[0] <= '9') {
        if (length + 2 > output.size()) {
            return false;
        }
        const auto used = static_cast<std::ptrdiff_t>(length);
        std::move_backward(output.begin(), output.begin() + used, output.begin() + used + 1);
        output[0] = '_';
        ++length;
    }
    output[length] = '\0';
    return true;
}

[[nodiscard]] bool controller_name(const sdk::Catalog& catalog,
                                   const format::Activity& activity,
                                   std::span<char> output) noexcept {
    std::array<char, 256> stem{};
    if (!controller_stem(catalog, activity, stem)) {
        return false;
    }
    const int length = std::snprintf(output.data(), output.size(), "%s.lua", stem.data());
    return length > 0 && static_cast<std::size_t>(length) < output.size();
}

/** Reads one whole file into the shared source buffer and says why it could not be read. */
[[nodiscard]] SourceStatus read_file(const core::path::Buffer& path,
                                     std::span<const char>& output) noexcept {
    output = {};
    HANDLE file = CreateFileW(path.chars.data(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SourceStatus::missing
                   : SourceStatus::fileError;
    }
    LARGE_INTEGER size{};
    const bool sizeReady = GetFileSizeEx(file, &size) != FALSE;
    if (!sizeReady || size.QuadPart <= 0
        || static_cast<unsigned long long>(size.QuadPart) > g_source.size()) {
        CloseHandle(file);
        return sizeReady ? SourceStatus::tooLarge : SourceStatus::fileError;
    }
    DWORD read = 0;
    const auto requested = static_cast<DWORD>(size.QuadPart);
    const bool loaded =
        ReadFile(file, g_source.data(), requested, &read, nullptr) != FALSE && read == requested;
    CloseHandle(file);
    if (!loaded) {
        return SourceStatus::fileError;
    }
    output = {g_source.data(), read};
    return SourceStatus::ready;
}

/** Reads the authored script for the activity, falling back to the generated one when absent. */
[[nodiscard]] SourceStatus read_source(const sdk::Catalog& catalog,
                                       const format::Activity& activity,
                                       std::span<const char>& output) noexcept {
    std::array<char, 260> authoredName{};
    if (!controller_name(catalog, activity, authoredName)) {
        return SourceStatus::fileError;
    }
    std::array<wchar_t, 260> wideName{};
    const int wideLength = MultiByteToWideChar(CP_UTF8,
                                               MB_ERR_INVALID_CHARS,
                                               authoredName.data(),
                                               -1,
                                               wideName.data(),
                                               static_cast<int>(wideName.size()));
    core::path::Buffer authoredPath = g_scriptRoot;
    if (wideLength <= 1 || !core::path::append(authoredPath, L"\\")
        || !core::path::append(authoredPath, wideName.data())) {
        return SourceStatus::fileError;
    }
    const SourceStatus authored = read_file(authoredPath, output);
    if (authored != SourceStatus::missing) {
        return authored;
    }

    core::path::Buffer generatedPath = g_scriptRoot;
    std::array<wchar_t, 96> generatedName{};
    const int generatedLength = _snwprintf_s(generatedName.data(),
                                             generatedName.size(),
                                             _TRUNCATE,
                                             L"\\activities\\a_%04u_%08x.lua",
                                             activity.activityIndex,
                                             activity.definitionHash);
    if (generatedLength <= 0
        || !core::path::append(generatedPath,
                               {generatedName.data(), static_cast<std::size_t>(generatedLength)})) {
        return SourceStatus::fileError;
    }
    return read_file(generatedPath, output);
}

/** Builds the exact durable program identity from the already validated SDK view. */
[[nodiscard]] bool make_program_key(const RuntimeInstance& instance,
                                    const format::Activity& activity,
                                    std::span<const char> source,
                                    mission_state::ProgramKey& output) noexcept {
    output = {};
    if (instance.view.catalog == nullptr
        || activity.activityIndex
               > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
        return false;
    }
    const std::span<const std::byte> digest = instance.view.catalog->sdk_build_sha256();
    if (digest.size() != output.sdkBuildSha256.size()) {
        return false;
    }
    std::copy(digest.begin(), digest.end(), output.sdkBuildSha256.begin());
    if (!sdk_bridge::world_program_generation_sha256(instance.worldView,
                                                     output.worldGenerationSha256)) {
        output = {};
        return false;
    }
    output.worldScenarioTag = instance.worldView.scenario_tag();
    if (!middleware::crypto::sha256::hash(std::as_bytes(source), output.scriptSourceSha256)) {
        output = {};
        return false;
    }
    output.activityDefinition = activity.definitionHash;
    output.activityIndex = static_cast<std::int16_t>(activity.activityIndex);
    output.publicTarget = instance.publicTarget;
    return true;
}

/** Binds and restores the durable record before the opened program enters a callback. */
[[nodiscard]] bool bind_mission_state(RuntimeInstance& instance, std::uint64_t now) noexcept {
    mission_state::Snapshot snapshot{};
    mission_state::Status status =
        mission_state::bind(instance.view.binding, instance.programKey, snapshot);
    ReloadAuthorization* const authorization = reload_authorization(instance.view.binding);
    if (status == mission_state::Status::programMismatch && authorization != nullptr) {
        status = mission_state::rebind_program(
            instance.view.binding, authorization->program, instance.programKey, snapshot);
    }
    if (authorization != nullptr) {
        *authorization = {};
    }
    note_vm_status(instance, "state", mission_state::status_name(status));
    if (status != mission_state::Status::ready) {
        log_line(
            core::log::Level::warn, &instance, "state_bind", mission_state::status_name(status));
        lua_vm::fault(instance.vm, "authoritative mission State binding was refused");
        return false;
    }
    instance.missionStateBound = true;
    accept_mission_state(instance, snapshot);
    if (snapshot.state.variableCount > snapshot.state.variables.size()
        || snapshot.state.timerCount > snapshot.state.timers.size()) {
        fault_instance(instance, "authoritative mission State durable row count is invalid");
        return false;
    }
    std::vector<lua_vm::Intent> restoredIntents{};
    try {
        restoredIntents.reserve(snapshot.state.pendingIntents.size());
        for (const mission_state::PendingIntent& pending : snapshot.state.pendingIntents) {
            restoredIntents.push_back(pending.value);
        }
    } catch (const std::bad_alloc&) {
        fault_instance(instance, "authoritative mission State restore allocation failed");
        return false;
    }
    if (!lua_vm::restore_state(instance.vm,
                               snapshot.state.phase,
                               snapshot.state.revision,
                               {snapshot.state.variables.data(), snapshot.state.variableCount},
                               {snapshot.state.timers.data(), snapshot.state.timerCount},
                               snapshot.state.nextTimerSequence,
                               snapshot.state.nextIntentKey,
                               restoredIntents)) {
        fault_instance(instance, "authoritative mission State restore was refused");
        log_line(core::log::Level::warn, &instance, "state_restore", "vm_refused");
        return false;
    }
    if (instance.durableHostOutputRevision != mission_state::kAbsentHostOutputRevision) {
        instance.expectedScriptableRevision = instance.durableHostOutputRevision;
        instance.deliveryStage = DeliveryStage::awaitingHostCommit;
        instance.deliveryDeadline = deadline_after(now, kHostCommitTimeoutMs);
        instance.firstIntentAttempt = now;
        instance.intentAttempts = 1;
        host::InstanceSnapshot hostView{};
        if (host::instance_snapshot(instance.view.binding, hostView) && hostView.outputPending
            && hostView.outputKind == host::OutputKind::scriptableOverride
            && hostView.scriptableRevision == instance.expectedScriptableRevision) {
            instance.deliveryStage = DeliveryStage::awaitingTransport;
            instance.deliveryDeadline = deadline_after(now, kTransportTimeoutMs);
        } else if (hostView.scriptableRevision == instance.expectedScriptableRevision
                   && !hostView.outputPending
                   && hostView.scriptableTransportRevision != instance.expectedScriptableRevision) {
            instance.deliveryDeadline = now;
        }
    }
    if (snapshot.state.faulted) {
        lua_vm::fault(instance.vm, "authoritative mission State is faulted");
        log_line(core::log::Level::warn, &instance, "state_restore", "faulted");
        return false;
    }
    log_line(core::log::Level::debug,
             &instance,
             "state_restore",
             snapshot.state.started ? "started" : "ready");
    return true;
}

/**
 * Finishes one on_load call after same-session VM reattachment.
 * A restore is not a state transition, so an unchanged candidate must not spend a revision.
 * @return True when the program is running.
 */
[[nodiscard]] bool apply_load(RuntimeInstance& instance, lua_vm::CallStatus loaded) noexcept {
    lua_vm::Snapshot diagnostics{};
    lua_vm::snapshot(instance.vm, diagnostics);
    if (loaded != lua_vm::CallStatus::committed && loaded != lua_vm::CallStatus::noHandler) {
        instance.programStatus = ProgramStatus::programError;
        log_line(core::log::Level::warn,
                 &instance,
                 "load",
                 lua_vm::status_name(loaded),
                 {},
                 diagnostics.lastError.data());
        persist_mission_fault(instance);
        return false;
    }
    if (diagnostics.stateRevision != instance.missionStateRevision
        && !commit_mission_state(instance, true, instance.lastMissionSequence)) {
        return false;
    }
    instance.lastLoggedRevision = instance.missionStateRevision;
    return true;
}

enum class InitialStateGate : std::uint8_t {
    ready,
    pending,
    failed,
};

/** Selects the program-declared state once, then waits for its exact roster revision to publish. */
[[nodiscard]] InitialStateGate initial_state_gate(RuntimeInstance& instance) noexcept {
    if (!instance.initialStateDeclared) {
        return InitialStateGate::ready;
    }
    activity_sdk_mission::Snapshot seed{};
    const activity_sdk_mission::Status status =
        instance.initialStateSelected ? activity_sdk_mission::query(instance.view, seed)
                                      : activity_sdk_mission::select_state(
                                            instance.view, instance.initialStateRegion, {}, seed);
    if (status == activity_sdk_mission::Status::outputBusy) {
        return InitialStateGate::pending;
    }
    if (status != activity_sdk_mission::Status::ready || !seed.configured
        || seed.plan.effectiveRegion != static_cast<std::uint32_t>(instance.initialStateRegion)) {
        log_line(core::log::Level::warn,
                 &instance,
                 "initial_state",
                 activity_sdk_mission::status_name(status));
        fault_instance(instance, "program initial_state mission-seed selection was refused");
        return InitialStateGate::failed;
    }
    instance.initialStateSelected = true;
    // `plan.effectiveRegion` is the authored-state key; several authored states share one client
    // slice-set region, so the client link cannot report it. The lease revision reaching the
    // transport is the publication acknowledgement.
    return seed.publicationPending || seed.revision == 0 || seed.publishedRevision != seed.revision
               ? InitialStateGate::pending
               : InitialStateGate::ready;
}

/** Runs and durably commits a fresh program only after its initial-state gate is open. */
[[nodiscard]] bool start_program(RuntimeInstance& instance, std::uint64_t now) noexcept {
    const lua_vm::CallStatus started = lua_vm::start(instance.vm, now);
    note_vm_status(instance, "start", lua_vm::status_name(started));
    if (started != lua_vm::CallStatus::committed && started != lua_vm::CallStatus::noHandler) {
        instance.programStatus = ProgramStatus::programError;
        lua_vm::Snapshot diagnostics{};
        lua_vm::snapshot(instance.vm, diagnostics);
        log_line(core::log::Level::warn,
                 &instance,
                 "start",
                 lua_vm::status_name(started),
                 {},
                 diagnostics.lastError.data());
        persist_mission_fault(instance);
        return false;
    }
    if (!commit_mission_state(instance, true, instance.lastMissionSequence)) {
        return false;
    }
    instance.startPending = false;
    instance.lastLoggedRevision = instance.missionStateRevision;
    log_line(core::log::Level::info, &instance, "open", "ready");
    return true;
}

/** Opens the program for one slot, binds its durable record, then reattaches or starts it. */
[[nodiscard]] AttachResult open_program(RuntimeInstance& instance, std::uint64_t now) noexcept {
    const format::Activity* const activity = sdk::bound_activity(instance.view);
    if (activity == nullptr
        || !sdk_bridge::program_identity(instance.view, instance.publicTarget, instance.identity)) {
        instance.programStatus = ProgramStatus::programError;
        note_vm_status(instance, "open", "invalid_sdk_view");
        log_line(core::log::Level::warn, &instance, "open", "invalid_sdk_view");
        return AttachResult::programError;
    }
    instance.identity.sdkLuaSearchPath = g_sdkLuaSearchPath;
    instance.identity.playerKey = instance.playerKey;
    std::span<const char> source{};
    switch (read_source(*instance.view.catalog, *activity, source)) {
    case SourceStatus::missing:
        instance.programStatus = ProgramStatus::missing;
        note_vm_status(instance, "open", "no_script");
        log_line(core::log::Level::info, &instance, "open", "no_script");
        return AttachResult::noScript;
    case SourceStatus::fileError:
        instance.programStatus = ProgramStatus::fileError;
        note_vm_status(instance, "open", "file_error");
        log_line(core::log::Level::warn, &instance, "open", "file_error");
        return AttachResult::scriptFileError;
    case SourceStatus::tooLarge:
        instance.programStatus = ProgramStatus::sourceTooLarge;
        note_vm_status(instance, "open", "source_too_large");
        log_line(core::log::Level::warn, &instance, "open", "source_too_large");
        return AttachResult::sourceTooLarge;
    case SourceStatus::ready:
        break;
    }
    if (!make_program_key(instance, *activity, source, instance.programKey)) {
        std::fill(
            g_source.begin(), g_source.begin() + static_cast<std::ptrdiff_t>(source.size()), '\0');
        instance.programStatus = ProgramStatus::programError;
        note_vm_status(instance, "state", "invalid_program_key");
        log_line(core::log::Level::warn, &instance, "open", "invalid_program_key");
        return AttachResult::programError;
    }
    const lua_vm::OpenStatus opened =
        lua_vm::open(instance.vm,
                     instance.identity,
                     sdk_bridge::definition_api(instance.view, instance.worldView),
                     source);
    std::fill(
        g_source.begin(), g_source.begin() + static_cast<std::ptrdiff_t>(source.size()), '\0');
    note_vm_status(instance, "open", lua_vm::status_name(opened));
    if (opened != lua_vm::OpenStatus::ready) {
        instance.programStatus = ProgramStatus::programError;
        lua_vm::Snapshot diagnostics{};
        lua_vm::snapshot(instance.vm, diagnostics);
        log_line(core::log::Level::warn,
                 &instance,
                 "open",
                 lua_vm::status_name(opened),
                 {},
                 diagnostics.lastError.data());
        return AttachResult::programError;
    }
    instance.programStatus = ProgramStatus::loaded;
    instance.initialStateDeclared =
        lua_vm::initial_state_region(instance.vm, instance.initialStateRegion);
    if (instance.initialStateDeclared) {
        instance.activeRegion = instance.initialStateRegion;
    }
    if (!bind_mission_state(instance, now)) {
        instance.programStatus = ProgramStatus::programError;
        return AttachResult::programError;
    }
    if (instance.missionStarted) {
        instance.missionReattached = true;
        const lua_vm::CallStatus loaded = lua_vm::load(instance.vm, now);
        note_vm_status(instance, "load", lua_vm::status_name(loaded));
        if (!apply_load(instance, loaded)) {
            return AttachResult::programError;
        }
        log_line(core::log::Level::info, &instance, "open", "ready", "reason=state_reattached");
        return AttachResult::ready;
    }
    switch (initial_state_gate(instance)) {
    case InitialStateGate::failed:
        return AttachResult::programError;
    case InitialStateGate::pending:
        instance.startPending = true;
        log_line(core::log::Level::info, &instance, "initial_state", "publication_pending");
        return AttachResult::ready;
    case InitialStateGate::ready:
        break;
    }
    return start_program(instance, now) ? AttachResult::ready : AttachResult::programError;
}

} // namespace

bool controller_file_name(std::uint32_t oneBasedActivityRow, std::span<char> output) noexcept {
    if (oneBasedActivityRow == 0 || output.empty()) {
        return false;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    if (catalog == nullptr || oneBasedActivityRow - 1 >= catalog->activities().size()) {
        return false;
    }
    return controller_name(*catalog, catalog->activities()[oneBasedActivityRow - 1], output);
}

/** Resolves the script root and the SDK Lua search path. Logs its own refusal. */
bool resolve_script_paths() noexcept {
    HMODULE const module = owning_module();
    core::path::Buffer sdkLua{};
    core::path::Buffer scriptLua{};
    // Generated modules come first, so a controller cannot shadow one by filename.
    if (module == nullptr || !core::path::artifact_directory(module, g_scriptRoot)
        || !core::path::artifact_directory(module, sdkLua)
        || !core::path::artifact_directory(module, scriptLua)
        || !core::path::append(sdkLua, L"\\sdk\\lua\\?.lua")
        || !core::path::append(scriptLua, L"\\scripts\\?.lua") || !core::path::append(sdkLua, L";")
        || !core::path::append(sdkLua, {scriptLua.chars.data(), scriptLua.length})
        || !core::path::append(g_scriptRoot, L"\\scripts")) {
        log_line(core::log::Level::warn, nullptr, "initialize", "path_error");
        return false;
    }
    const int sdkLuaBytes = WideCharToMultiByte(CP_UTF8,
                                                WC_ERR_INVALID_CHARS,
                                                sdkLua.chars.data(),
                                                -1,
                                                g_sdkLuaSearchPath.data(),
                                                static_cast<int>(g_sdkLuaSearchPath.size()),
                                                nullptr,
                                                nullptr);
    if (sdkLuaBytes <= 1) {
        g_sdkLuaSearchPath = {};
        log_line(core::log::Level::warn, nullptr, "initialize", "sdk_lua_path_error");
        return false;
    }
    return true;
}

/** Clears the script buffer, both paths and every reload authorization. */
void clear_script_paths() noexcept {
    std::fill(g_source.begin(), g_source.end(), '\0');
    g_scriptRoot = {};
    g_sdkLuaSearchPath = {};
    g_reloadAuthorizations = {};
}

/** @return False when no authorization slot is free, so the reload cannot replace this program. */
bool authorize_reload(const RuntimeInstance& instance) noexcept {
    ReloadAuthorization* const authorization = free_reload_authorization();
    if (authorization == nullptr) {
        return false;
    }
    authorization->binding = instance.view.binding;
    authorization->program = instance.programKey;
    authorization->occupied = true;
    return true;
}

/** Binds one host instance to a free slot once its link, SDK view and world view all resolve. */
void attach_instance(const host::InstanceSnapshot& hostInstance,
                     const sdk::Snapshot& catalog,
                     std::uint64_t now) noexcept {
    if (find_instance(hostInstance.binding) != nullptr) {
        return;
    }
    if (catalog == nullptr) {
        report_attach_result(
            hostInstance.binding, AttachResult::catalogUnavailable, "catalog_unavailable");
        return;
    }
    server::bap::ActivityLinkView link{};
    if (!server::bap::activity_link_view(hostInstance.binding, link)) {
        report_attach_result(
            hostInstance.binding, AttachResult::noActivityLink, "no_activity_link");
        return;
    }
    if (!link.joined) {
        report_attach_result(
            hostInstance.binding, AttachResult::noActivityLink, "activity_join_pending");
        return;
    }
    sdk::BoundView view{};
    const sdk::Selection selection{
        .binding = hostInstance.binding,
        .matchingLinks = link.matchingLinks,
        .activityClientGeneration = link.activityClientGeneration,
    };
    const sdk::Status status = sdk::resolve(catalog, selection, view);
    if (status != sdk::Status::ready) {
        report_attach_result(hostInstance.binding,
                             AttachResult::sdkStatus,
                             sdk::status_name(status),
                             format::kAbsentIndex,
                             status);
        return;
    }
    generated::GeneratedWorldView worldView{};
    const generated::BindStatus worldStatus = generated::resolve(view, worldView);
    if (worldStatus != generated::BindStatus::ready) {
        report_attach_result(hostInstance.binding,
                             AttachResult::generatedWorldStatus,
                             generated::status_name(worldStatus),
                             view.activityRow,
                             sdk::Status::notReady,
                             worldStatus);
        return;
    }
    RuntimeInstance* const instance = free_instance();
    if (instance == nullptr) {
        report_attach_result(
            hostInstance.binding, AttachResult::capacity, "capacity", view.activityRow);
        return;
    }
    instance->view = std::move(view);
    instance->worldView = std::move(worldView);
    instance->publicTarget = link.publicTarget;
    instance->playerKey = link.playerKey;
    instance->occupied = true;
    const AttachResult opened = open_program(*instance, now);
    report_attach_result(
        hostInstance.binding, opened, attach_result_name(opened), instance->view.activityRow);
}

/** Drops slots that no longer match, publishes the roster, and attaches active host instances. */
void synchronize_instances(std::uint64_t now) noexcept {
    host::DiagnosticsSnapshot diagnostics{};
    host::snapshot(diagnostics);
    retire_attach_diagnostics(diagnostics);
    retire_unbound_pending_events();
    for (RuntimeInstance& instance : g_instances) {
        const bool bindingActive =
            instance.occupied && is_active(diagnostics, instance.view.binding);
        const bool bindingRetained =
            instance.occupied && state::activity::binding_matches(instance.view.binding);
        // A generation change only stales the view, so rebind and keep the program.
        if (instance.occupied && bindingActive && bindingRetained && !still_exact(instance)
            && rebind_instance(instance)) {
            log_line(core::log::Level::debug, &instance, "rebind", "generation");
            continue;
        }
        if (instance.occupied && (!bindingActive || !still_exact(instance))) {
            log_line(core::log::Level::info, &instance, "close", "stale_generation");
            // Accepted mission inputs belong to the exact SessionBinding, not one ActivityClient
            // generation or one temporary link outage. Clear only after State replaces the exact
            // session generation; otherwise reattach must finish every already-accepted row.
            clear_instance(instance, !bindingRetained);
        }
    }
    std::array<state::activity::SessionRosterRow, state::activity::kSessionCapacity> roster{};
    std::size_t rosterCount = 0;
    static_cast<void>(state::activity::snapshot_session_roster(roster, rosterCount));
    for (RuntimeInstance& instance : g_instances) {
        if (instance.occupied) {
            push_session_roster_edges(instance, {roster.data(), rosterCount});
        }
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    publish_fireteam_life(now);
    for (std::size_t index = 0; index < diagnostics.instanceCount; ++index) {
        if (diagnostics.instances[index].active) {
            attach_instance(diagnostics.instances[index], catalog, now);
        }
    }
}

/** Advances fresh programs only when their declared state roster has reached transport output. */
void service_pending_starts(std::uint64_t now) noexcept {
    for (RuntimeInstance& instance : g_instances) {
        if (!instance.occupied || !instance.startPending
            || instance.programStatus != ProgramStatus::loaded || instance.missionStarted) {
            continue;
        }
        switch (initial_state_gate(instance)) {
        case InitialStateGate::pending:
            break;
        case InitialStateGate::failed:
            instance.startPending = false;
            break;
        case InitialStateGate::ready:
            static_cast<void>(start_program(instance, now));
            break;
        }
    }
}

/** Probes the mission script for the activity to apply its initial_state slice-set override. */
void apply_script_initial_state_override(state::activity::destination::DestinationSelection& selection) noexcept {
    if (selection.activityIndex < 0) {
        return;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    if (catalog == nullptr) {
        return;
    }
    const format::Activity* activity = nullptr;
    for (const auto& a : catalog->activities()) {
        if (static_cast<std::int32_t>(a.activityIndex) == static_cast<std::int32_t>(selection.activityIndex)) {
            activity = &a;
            break;
        }
    }
    if (activity == nullptr) {
        return;
    }
    std::span<const char> source;
    if (read_source(*catalog, *activity, source) != SourceStatus::ready) {
        return;
    }
    const std::int32_t region = lua_vm::probe_initial_state_region(source, g_sdkLuaSearchPath.data());
    if (region >= 0) {
        selection.sliceSetOverride = static_cast<std::uint16_t>(region);
        selection.hasSliceSetOverride = true;
    }
}

} // namespace sunrise::server::activity::mission
