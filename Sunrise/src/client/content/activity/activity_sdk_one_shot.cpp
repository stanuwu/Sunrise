#include "activity_sdk_one_shot.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "../../../state/content_manifest/content_manifest_state_runtime.h"
#include "../items/packages/internal.h"
#include "activity_sdk_estate_validation.h"
#include "activity_sdk_tree_publication.h"

namespace sunrise::client::content::activity::sdk_generation::one_shot {
namespace {

namespace generated = state::activity_sdk::generated_world;
namespace publication = tree_publication;
namespace estate = estate_validation;

// A stage name carries three sequence words so two passes never collide.
constexpr std::wstring_view kStageSuffix = L".stage.%08lX.%08lX.%08lX";

volatile LONG g_stageSequence{};

[[nodiscard]] bool cancelled(OfflineCancelProbe probe, void* context) noexcept {
    return probe != nullptr && probe(context);
}

[[nodiscard]] bool copy_fingerprint(void* opaque,
                                    const state::content_manifest::View& view) noexcept {
    if (opaque == nullptr || view.buildFingerprint.size() != generated::Digest{}.size()) {
        return false;
    }
    auto& output = *static_cast<generated::Digest*>(opaque);
    std::copy(view.buildFingerprint.begin(), view.buildFingerprint.end(), output.begin());
    return true;
}

void report(OfflineProgressSink sink,
            void* context,
            state::activity_sdk::generation::Status status,
            std::uint32_t current,
            std::uint32_t total,
            std::string_view detail) noexcept {
    if (sink != nullptr) {
        sink(context, {status, current, total, 0, detail});
    }
}

[[nodiscard]] bool equal_path(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size()
           && CompareStringOrdinal(left.data(),
                                   static_cast<int>(left.size()),
                                   right.data(),
                                   static_cast<int>(right.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

[[nodiscard]] bool path_prefix(std::wstring_view parent, std::wstring_view child) noexcept {
    return child.size() > parent.size() && child[parent.size()] == L'\\'
           && CompareStringOrdinal(parent.data(),
                                   static_cast<int>(parent.size()),
                                   child.data(),
                                   static_cast<int>(parent.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

[[nodiscard]] bool overlaps(std::wstring_view left, std::wstring_view right) noexcept {
    return equal_path(left, right) || path_prefix(left, right) || path_prefix(right, left);
}

/** Resolves one bounded view through an owned null-terminated Win32 input. */
[[nodiscard]] bool full_path(std::wstring_view input, std::wstring& output) noexcept {
    output.clear();
    if (input.empty() || input.size() > (std::numeric_limits<DWORD>::max)() - 1U) {
        return false;
    }
    try {
        const std::wstring terminated(input);
        const DWORD needed = GetFullPathNameW(terminated.c_str(), 0, nullptr, nullptr);
        if (needed == 0) {
            return false;
        }
        std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(terminated.c_str(), needed, buffer.data(), nullptr);
        if (written == 0 || written >= needed) {
            return false;
        }
        buffer.resize(written);
        while (buffer.size() > 3U && (buffer.back() == L'\\' || buffer.back() == L'/')) {
            buffer.pop_back();
        }
        output = std::move(buffer);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Requires every existing drive-path directory component to be ordinary. */
[[nodiscard]] bool ordinary_ancestry(const std::wstring& directory) noexcept {
    if (directory.size() < 3U || directory[1] != L':' || directory[2] != L'\\') {
        return false;
    }
    try {
        std::size_t cursor = 3U;
        while (true) {
            const std::size_t separator = directory.find(L'\\', cursor);
            const std::size_t end = separator == std::wstring::npos ? directory.size() : separator;
            const std::wstring prefix = directory.substr(0, end);
            const DWORD attributes = GetFileAttributesW(prefix.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES
                || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return false;
            }
            if (separator == std::wstring::npos) {
                return true;
            }
            cursor = separator + 1U;
        }
    } catch (...) {
        return false;
    }
}

/** Canonicalizes one existing directory. */
[[nodiscard]] bool final_directory_path(std::wstring_view input, std::wstring& output) noexcept {
    std::wstring lexical;
    if (!full_path(input, lexical) || !ordinary_ancestry(lexical)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(lexical.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    const HANDLE directory = CreateFileW(lexical.c_str(),
                                         FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS,
                                         nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD needed =
        GetFinalPathNameByHandleW(directory, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    bool complete = false;
    if (needed != 0) {
        try {
            std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
            const DWORD written = GetFinalPathNameByHandleW(
                directory, buffer.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (written != 0 && written < needed) {
                buffer.resize(written);
                // Windows returns the extended prefix; callers store the plain path.
                constexpr std::wstring_view kExtended = L"\\\\?\\";
                if (buffer.starts_with(kExtended)) {
                    buffer.erase(0, kExtended.size());
                }
                while (buffer.size() > 3U && (buffer.back() == L'\\' || buffer.back() == L'/')) {
                    buffer.pop_back();
                }
                output = std::move(buffer);
                complete = true;
            }
        } catch (...) {
            complete = false;
        }
    }
    (void)CloseHandle(directory);
    return complete;
}

/** Canonicalizes an existing output or a missing direct child of an ordinary parent. */
[[nodiscard]] bool canonical_output(std::wstring_view input, std::wstring& output) noexcept {
    std::wstring lexical;
    if (!full_path(input, lexical) || lexical.size() <= 3U) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(lexical.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
               && final_directory_path(lexical, output);
    }
    const std::size_t separator = lexical.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator + 1 >= lexical.size()) {
        return false;
    }
    std::wstring parent;
    if (!final_directory_path(std::wstring_view(lexical).substr(0, separator), parent)) {
        return false;
    }
    try {
        output = parent;
        output.push_back(L'\\');
        output.append(lexical.substr(separator + 1));
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Allocates one process-unique sibling used exclusively by the current generation. */
[[nodiscard]] bool allocate_stage(const std::wstring& output, std::wstring& stage) noexcept {
    std::array<wchar_t, 80> suffix{};
    const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_stageSequence));
    const int length = std::swprintf(suffix.data(),
                                     suffix.size(),
                                     kStageSuffix.data(),
                                     GetCurrentProcessId(),
                                     GetCurrentThreadId(),
                                     sequence);
    if (length <= 0 || static_cast<std::size_t>(length) >= suffix.size()) {
        return false;
    }
    try {
        stage = output;
        stage.append(suffix.data(), static_cast<std::size_t>(length));
    } catch (...) {
        stage.clear();
        return false;
    }
    return CreateDirectoryW(stage.c_str(), nullptr) != FALSE;
}

/** Maps the retained-result layer without exposing its internal status partitions. */
[[nodiscard]] Status map_estate_status(estate::Status value) noexcept {
    switch (value) {
    case estate::Status::ready:
        return Status::ready;
    case estate::Status::cancelled:
        return Status::cancelled;
    case estate::Status::invalidInput:
        return Status::invalidInput;
    case estate::Status::notGenerated:
        return Status::notGenerated;
    case estate::Status::publication:
        return Status::publication;
    }
    return Status::notGenerated;
}

/** Copies one retained generation result into the process-local result contract. */
void copy_estate_result(const estate::Result& source, Result& output) noexcept {
    output = {};
    output.build.scenarioCount = source.scenarioCount;
    output.build.activityRootCount = source.activityRootCount;
    output.build.activityCount = source.activityCount;
    output.build.builtScenarioCount = source.builtScenarioCount;
    output.build.reusedScenarioCount = source.reusedScenarioCount;
    output.payloadSha256 = source.payloadSha256;
    output.packBytes = source.packBytes;
    output.luaFiles = source.luaFileCount;
}

} // namespace

/** Returns one bounded diagnostic name without including any source or reader material. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::ready:
        return "ready";
    case Status::cancelled:
        return "cancelled";
    case Status::busy:
        return "busy";
    case Status::invalidInput:
        return "invalid_input";
    case Status::stageAllocation:
        return "stage_allocation";
    case Status::generation:
        return "generation";
    case Status::notGenerated:
        return "not_generated";
    case Status::publication:
        return "publication";
    }
    return "invalid_input";
}

/** Runs the internal borrowed-source generator and publishes its isolated tree once. */
Status run(const Request& request,
           OfflineCancelProbe cancel,
           void* cancelContext,
           OfflineProgressSink progress,
           void* progressContext,
           Result& output) noexcept {
    output = {};
    std::wstring packages;
    std::wstring target;
    if (!final_directory_path(request.packageDirectory, packages)
        || !canonical_output(request.outputArtifactDirectory, target)
        || overlaps(packages, target)) {
        return Status::invalidInput;
    }
    estate::forget_canonical(target);
    std::wstring stage;
    if (!allocate_stage(target, stage)) {
        return Status::stageAllocation;
    }
    const OfflineBuildRequest buildRequest{
        request.sourceFingerprint, request.keys, packages, {}, stage};
    OfflineBuildResult build{};
    const OfflineBuildStatus built =
        build_offline(buildRequest, cancel, cancelContext, progress, progressContext, build);
    if (built != OfflineBuildStatus::ready) {
        (void)publication::discard(stage.c_str());
        switch (built) {
        case OfflineBuildStatus::cancelled:
            return Status::cancelled;
        case OfflineBuildStatus::busy:
            return Status::busy;
        case OfflineBuildStatus::invalidInput:
            return Status::invalidInput;
        case OfflineBuildStatus::ready:
            break;
        case OfflineBuildStatus::failed:
            return Status::generation;
        }
        return Status::generation;
    }
    if (cancelled(cancel, cancelContext)) {
        (void)publication::discard(stage.c_str());
        return Status::cancelled;
    }
    report(progress,
           progressContext,
           state::activity_sdk::generation::Status::publishing,
           build.scenarioCount,
           build.scenarioCount,
           "replacing complete generated SDK tree");
    const publication::Status publicationStatus =
        publication::publish(stage.c_str(), target.c_str(), nullptr, nullptr);
    if (publicationStatus != publication::Status::ready) {
        (void)publication::discard(stage.c_str());
        return Status::publication;
    }
    output.build = build;
    output.payloadSha256 = build.payloadSha256;
    output.packBytes = build.packBytes;
    output.luaFiles = build.luaFiles;
    const estate::Result retained{build.scenarioCount,
                                  build.activityRootCount,
                                  build.activityCount,
                                  build.builtScenarioCount,
                                  build.reusedScenarioCount,
                                  build.luaFiles,
                                  build.payloadSha256,
                                  build.packBytes};
    estate::remember_canonical(std::move(target), request.sourceFingerprint, retained);
    report(progress,
           progressContext,
           state::activity_sdk::generation::Status::ready,
           build.scenarioCount,
           build.scenarioCount,
           "generated SDK tree is ready");
    return Status::ready;
}

/** Acquires the adopted process-local source, runs once, and securely clears its sole key copy. */
Status run_process_local(std::wstring_view packageDirectory,
                         std::wstring_view cacheArtifactDirectory,
                         std::wstring_view outputArtifactDirectory,
                         OfflineCancelProbe cancel,
                         void* cancelContext,
                         OfflineProgressSink progress,
                         void* progressContext,
                         Result& output) noexcept {
    output = {};
    static_cast<void>(cacheArtifactDirectory);
    generated::Digest fingerprint{};
    middleware::content::packages::reader::BlockKeys keys{};
    if (!state::content_manifest::visit_snapshot(&copy_fingerprint, &fingerprint)
        || !items::packages::collect_keys(keys)) {
        SecureZeroMemory(&keys, sizeof keys);
        return Status::invalidInput;
    }
    const Request request{fingerprint, &keys, packageDirectory, {}, outputArtifactDirectory};
    const Status status = run(request, cancel, cancelContext, progress, progressContext, output);
    SecureZeroMemory(&keys, sizeof keys);
    return status;
}

/** Returns one process-local result without reopening generated files. */
Status accept_generated(std::wstring_view artifactDirectory,
                        const generated::Digest& sourceFingerprint,
                        OfflineCancelProbe cancel,
                        void* cancelContext,
                        Result& output) noexcept {
    output = {};
    estate::Result retained{};
    const estate::Status status = estate::accept_generated(
        artifactDirectory, sourceFingerprint, nullptr, cancel, cancelContext, retained);
    if (status == estate::Status::ready) {
        copy_estate_result(retained, output);
    }
    return map_estate_status(status);
}

/** Publishes one retained sibling without reopening generated files. */
Status publish_generated(std::wstring_view stageArtifactDirectory,
                         std::wstring_view outputArtifactDirectory,
                         const generated::Digest& sourceFingerprint,
                         OfflineCancelProbe cancel,
                         void* cancelContext,
                         Result& output) noexcept {
    output = {};
    estate::Result retained{};
    const estate::Status status = estate::publish_generated(stageArtifactDirectory,
                                                            outputArtifactDirectory,
                                                            sourceFingerprint,
                                                            cancel,
                                                            cancelContext,
                                                            retained);
    if (status == estate::Status::ready) {
        copy_estate_result(retained, output);
    }
    return map_estate_status(status);
}

} // namespace sunrise::client::content::activity::sdk_generation::one_shot
