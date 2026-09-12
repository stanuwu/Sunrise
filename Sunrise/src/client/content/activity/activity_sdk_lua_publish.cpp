#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../state/activity_sdk/runtime.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts {
namespace {

// Published Lua tree layout below the artifact root.
constexpr std::wstring_view kLuaSuffix = L"\\lua";
constexpr std::wstring_view kActivitiesSuffix = L"\\activities";
constexpr std::wstring_view kMissionsSuffix = L"\\missions";
constexpr std::wstring_view kSunriseSuffix = L"\\sunrise";
constexpr std::wstring_view kTemporarySuffix = L".%08lX.%08lX.%08lX.tmp";
// Distinct temporary names tried before publication fails.
constexpr std::size_t kTemporaryAttempts = 16;

volatile LONG g_temporarySequence{};

/** One small handle owner keeps failed publication paths closed. */
class FileHandle final {
public:
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] bool close() noexcept {
        if (value_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return CloseHandle(value) != FALSE;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] bool ensure_directory(const wchar_t* path) noexcept {
    if (CreateDirectoryW(path, nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[nodiscard]] bool child_path(std::wstring_view parent,
                              std::wstring_view suffix,
                              core::path::Buffer& output) noexcept {
    return core::path::assign(output, parent) && core::path::append(output, suffix);
}

/** Forms a process-, thread-, and sequence-scoped sibling name for publication. */
[[nodiscard]] bool temporary_path(const wchar_t* finalPath, core::path::Buffer& output) noexcept {
    std::array<wchar_t, 64> suffix{};
    const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_temporarySequence));
    const int length = std::swprintf(suffix.data(),
                                     suffix.size(),
                                     kTemporarySuffix.data(),
                                     GetCurrentProcessId(),
                                     GetCurrentThreadId(),
                                     sequence);
    return length > 0 && static_cast<std::size_t>(length) < suffix.size()
           && core::path::assign(output, finalPath)
           && core::path::append(output, std::wstring_view(suffix.data(), length));
}

/** Writes the complete source in DWORD-sized chunks and rejects short writes. */
[[nodiscard]] bool write_all(HANDLE file, std::string_view source) noexcept {
    while (!source.empty()) {
        const std::size_t count =
            (std::min)(source.size(),
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
        DWORD transferred = 0;
        if (WriteFile(file, source.data(), static_cast<DWORD>(count), &transferred, nullptr)
                == FALSE
            || transferred != count) {
            return false;
        }
        source.remove_prefix(count);
    }
    return true;
}

/** Publishes only a completely written, closed sibling file and removes it after failure. */
[[nodiscard]] bool publish_file(const wchar_t* finalPath, std::string_view source) noexcept {
    core::path::remove_stale_siblings(finalPath);
    for (std::size_t attempt = 0; attempt < kTemporaryAttempts; ++attempt) {
        core::path::Buffer temporary{};
        if (!temporary_path(finalPath, temporary)) {
            return false;
        }
        const HANDLE raw = CreateFileW(temporary.chars.data(),
                                       GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return false;
        }
        FileHandle file(raw);
        bool complete = write_all(file.get(), source);
        complete = file.close() && complete;
        if (!complete || !core::path::publish_sibling(temporary.chars.data(), finalPath)) {
            (void)DeleteFileW(temporary.chars.data());
            return false;
        }
        return true;
    }
    return false;
}

[[nodiscard]] bool add_bytes(std::uint64_t& total, std::string_view source) noexcept {
    if (source.size() > (std::numeric_limits<std::uint64_t>::max)() - total) {
        return false;
    }
    total += source.size();
    return true;
}

/** Reads one ordinary file only when its byte count matches the expected commit record. */
[[nodiscard]] bool file_equals(const wchar_t* path, std::string_view expected) noexcept {
    if (path == nullptr || expected.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    std::string actual;
    bool complete = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
                    && static_cast<std::uint64_t>(size.QuadPart) == expected.size();
    if (complete) {
        try {
            actual.resize(expected.size());
        } catch (...) {
            complete = false;
        }
    }
    DWORD read = 0;
    if (complete) {
        complete = ReadFile(file, actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr)
                       != FALSE
                   && read == actual.size();
    }
    complete = CloseHandle(file) != FALSE && complete;
    return complete && actual == expected;
}

/** Copies one catalog digest into the fixed generator source field. */
[[nodiscard]] bool copy_digest(std::span<const std::byte> input,
                               std::array<std::byte, 32>& output) noexcept {
    if (input.size() != output.size()) {
        return false;
    }
    std::copy(input.begin(), input.end(), output.begin());
    return true;
}

[[nodiscard]] bool publish_named(std::wstring_view directory,
                                 std::wstring_view name,
                                 std::string_view source,
                                 std::uint64_t& bytes) noexcept {
    core::path::Buffer path{};
    return child_path(directory, name, path) && publish_file(path.chars.data(), source)
           && add_bytes(bytes, source);
}

/** Converts one generator-owned ASCII stem to a child Lua filename. */
[[nodiscard]] bool module_name(const SourceModule& module, std::wstring& output) {
    if (module.stem.empty()) {
        return false;
    }
    output.clear();
    output.reserve(module.stem.size() + 5U);
    output.push_back(L'\\');
    for (const unsigned char byte : module.stem) {
        if (byte >= 0x80U || (std::isalnum(byte) == 0 && byte != '_')) {
            return false;
        }
        output.push_back(static_cast<wchar_t>(byte));
    }
    output.append(L".lua");
    return true;
}

/** Shared immutable module spans and atomic scheduling state for one file batch. */
struct ModulePublishBatch final {
    std::wstring_view activityDirectory{};
    std::wstring_view missionDirectory{};
    std::span<const SourceModule> activities{};
    std::span<const SourceModule> missions{};
    std::atomic_size_t next{};
    std::atomic_uint64_t bytes{};
    std::atomic_bool failed{};
};

/** Uses most cores while leaving two for the running game and server. */
[[nodiscard]] std::size_t publish_worker_count(std::size_t files) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::size_t processors = static_cast<std::size_t>(info.dwNumberOfProcessors);
    const std::size_t available = processors > 2U ? processors - 2U : 1U;
    return (std::min)(files, (std::min)(available, static_cast<std::size_t>(12)));
}

/** Publishes distinct module paths without serializing independent file writes. */
void run_module_publish_worker(ModulePublishBatch& batch) noexcept {
    std::wstring fileName;
    std::uint64_t bytes = 0;
    const std::size_t total = batch.activities.size() + batch.missions.size();
    while (!batch.failed.load(std::memory_order_relaxed)) {
        const std::size_t index = batch.next.fetch_add(1U);
        if (index >= total) {
            break;
        }
        const bool activity = index < batch.activities.size();
        const SourceModule& module =
            activity ? batch.activities[index] : batch.missions[index - batch.activities.size()];
        const std::wstring_view directory =
            activity ? batch.activityDirectory : batch.missionDirectory;
        if (!module_name(module, fileName)
            || !publish_named(directory, fileName, module.source, bytes)) {
            batch.failed.store(true, std::memory_order_relaxed);
            break;
        }
    }
    batch.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

/** Adapts one file publisher to the Windows thread ABI. */
DWORD WINAPI module_publish_thread_main(void* opaque) noexcept {
    run_module_publish_worker(*static_cast<ModulePublishBatch*>(opaque));
    return 0;
}

/** Publishes all generated modules in parallel and returns their byte count. */
[[nodiscard]] bool publish_modules(std::wstring_view activityDirectory,
                                   std::wstring_view missionDirectory,
                                   const Bundle& bundle,
                                   std::uint64_t& bytes) {
    ModulePublishBatch batch{};
    batch.activityDirectory = activityDirectory;
    batch.missionDirectory = missionDirectory;
    batch.activities = bundle.activityModules;
    batch.missions = bundle.missionModules;
    const std::size_t total = batch.activities.size() + batch.missions.size();
    const std::size_t workers = publish_worker_count(total);
    if (workers == 0) {
        return false;
    }
    std::vector<HANDLE> threads;
    threads.reserve(workers - 1U);
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const HANDLE thread =
            CreateThread(nullptr, 0, &module_publish_thread_main, &batch, 0, nullptr);
        if (thread != nullptr) {
            threads.push_back(thread);
        }
    }
    run_module_publish_worker(batch);
    for (const HANDLE thread : threads) {
        (void)WaitForSingleObject(thread, INFINITE);
        (void)CloseHandle(thread);
    }
    if (batch.failed.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::uint64_t moduleBytes = batch.bytes.load(std::memory_order_relaxed);
    if (moduleBytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
        return false;
    }
    bytes += moduleBytes;
    return true;
}

} // namespace

/** Publishes each file atomically; a later failure can leave earlier files updated. */
Status publish_bundle(const wchar_t* sdkDirectory, const Bundle& bundle, Result& output) noexcept {
    output = {};
    if (sdkDirectory == nullptr || sdkDirectory[0] == L'\0' || bundle.activityIndex.empty()
        || bundle.missionIndex.empty() || bundle.activitySdkModule.empty()
        || bundle.behaviorModule.empty() || bundle.manifestJson.empty()
        || bundle.activityModules.empty() || bundle.missionModules.empty()) {
        return Status::invalidInput;
    }
    try {
        core::path::Buffer lua{};
        core::path::Buffer activities{};
        core::path::Buffer missions{};
        core::path::Buffer sunrise{};
        if (!child_path(sdkDirectory, kLuaSuffix, lua)
            || !child_path(lua.chars.data(), kActivitiesSuffix, activities)
            || !child_path(lua.chars.data(), kMissionsSuffix, missions)
            || !child_path(lua.chars.data(), kSunriseSuffix, sunrise)
            || !ensure_directory(lua.chars.data()) || !ensure_directory(activities.chars.data())
            || !ensure_directory(missions.chars.data())
            || !ensure_directory(sunrise.chars.data())) {
            return Status::directoryFailure;
        }

        std::uint64_t bytes = 0;
        if (!publish_named(lua.chars.data(), L"\\manifest.json", bundle.manifestJson, bytes)
            || !publish_named(
                sunrise.chars.data(), L"\\activity_sdk.lua", bundle.activitySdkModule, bytes)
            || !publish_named(
                sunrise.chars.data(), L"\\behaviors.lua", bundle.behaviorModule, bytes)) {
            return Status::writeFailure;
        }

        if (!publish_modules(activities.chars.data(), missions.chars.data(), bundle, bytes)) {
            return Status::writeFailure;
        }

        if (!publish_named(lua.chars.data(), L"\\missions.lua", bundle.missionIndex, bytes)
            || !publish_named(lua.chars.data(), L"\\activities.lua", bundle.activityIndex, bytes)) {
            return Status::writeFailure;
        }
        const std::size_t sourceFiles =
            bundle.activityModules.size() + bundle.missionModules.size();
        if (sourceFiles > format::kAbsentIndex - 5U) {
            return Status::writeFailure;
        }
        output.activityCount = static_cast<std::uint32_t>(bundle.activityModules.size());
        output.fileCount = static_cast<std::uint32_t>(sourceFiles + 5U);
        output.byteCount = bytes;
        return Status::ready;
    } catch (...) {
        output = {};
        return Status::writeFailure;
    }
}

Status publish(const wchar_t* sdkDirectory, const Source& source, Result& output) noexcept {
    output = {};
    Bundle bundle{};
    const Status built = build(source, bundle);
    return built == Status::ready ? publish_bundle(sdkDirectory, bundle, output) : built;
}

/** @return True when the published Lua modules already match this catalog. */
bool is_current(const wchar_t* sdkDirectory, const state::activity_sdk::Catalog& catalog) noexcept {
    if (sdkDirectory == nullptr || sdkDirectory[0] == L'\0') {
        return false;
    }
    try {
        Source source{};
        if (!copy_digest(catalog.sdk_build_sha256(), source.sdkBuildSha256)
            || !copy_digest(catalog.payload_sha256(), source.sdkPayloadSha256)
            || !copy_digest(catalog.content_key_sha256(), source.contentKeySha256)
            || !copy_digest(catalog.logical_ir_sha256(), source.logicalIrSha256)) {
            return false;
        }
        source.activities = catalog.activities();
        source.states = catalog.states();
        source.slots = catalog.slots();
        source.squads = catalog.squads();
        source.authoredSceneResources = catalog.authored_scene_resources();
        source.authoredSceneSquadEdges = catalog.authored_scene_squad_edges();
        source.taskTargets = catalog.task_targets();
        source.dialogueCueTexts = catalog.dialogue_cue_texts();
        source.directiveElements = catalog.directive_elements();
        source.behaviorPrograms = catalog.behavior_programs();
        source.behaviorInputs = catalog.behavior_inputs();
        source.behaviorChannelWrites = catalog.behavior_channel_writes();
        source.behaviorOwners = catalog.behavior_owners();
        source.behaviorActivityBindings = catalog.behavior_activity_bindings();
        source.actorClasses = catalog.actor_classes();
        source.actorMessageSchemas = catalog.actor_message_schemas();
        source.actorCommandDefinitions = catalog.actor_command_definitions();
        source.actorBehaviorProfiles = catalog.actor_behavior_profiles();
        source.simulationEventDefinitions = catalog.simulation_event_definitions();
        source.runtimeSchemas = catalog.runtime_schemas();
        source.runtimeFields = catalog.runtime_fields();
        source.runtimeTypeDefinitions = catalog.runtime_type_definitions();
        source.sobjectRsats = catalog.sobject_rsats();
        source.sobjectRsatDescriptors = catalog.sobject_rsat_descriptors();
        source.entityTypeDefinitions = catalog.entity_type_definitions();
        source.sobjectRsatFieldBindings = catalog.sobject_rsat_field_bindings();
        source.actorStateNames = catalog.actor_state_names();
        source.actorSequenceTables = catalog.actor_sequence_tables();
        source.actorSequenceEntries = catalog.actor_sequence_entries();
        source.actorSequenceBindings = catalog.actor_sequence_bindings();
        Bundle expected{};
        if (!internal::render_contract_files(source, expected)) {
            return false;
        }
        core::path::Buffer lua{};
        core::path::Buffer manifest{};
        return child_path(sdkDirectory, kLuaSuffix, lua)
               && child_path(lua.chars.data(), L"\\manifest.json", manifest)
               && file_equals(manifest.chars.data(), expected.manifestJson);
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts
