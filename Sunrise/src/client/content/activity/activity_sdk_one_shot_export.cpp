#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "activity_sdk_one_shot.h"
#include "activity_sdk_one_shot_abi.h"

namespace sunrise::client::content::activity::sdk_generation::one_shot {
namespace {

namespace public_abi = abi;
namespace generated = state::activity_sdk::generated_world;

struct ProgressAdapter final {
    public_abi::ProgressSink sink{};
    void* context{};
};

/** Forwards one offline build progress report to the caller's sink. */
void forward_progress(void* opaque, const OfflineBuildProgress& source) noexcept {
    if (opaque == nullptr) {
        return;
    }
    const auto& adapter = *static_cast<const ProgressAdapter*>(opaque);
    if (adapter.sink == nullptr) {
        return;
    }
    const public_abi::Progress progress{public_abi::kVersion,
                                        static_cast<std::uint32_t>(source.status),
                                        source.current,
                                        source.total,
                                        source.scenarioTag,
                                        source.detail.data(),
                                        static_cast<std::uint32_t>(source.detail.size())};
    adapter.sink(adapter.context, &progress);
}

/** Copies one internal result into the stable exported ABI with deterministic padding. */
void copy_result(Status status, const Result& source, public_abi::Result& output) noexcept {
    output = {};
    output.abiVersion = public_abi::kVersion;
    output.status = static_cast<std::uint32_t>(status);
    output.scenarioCount = source.build.scenarioCount;
    output.activityRootCount = source.build.activityRootCount;
    output.activityCount = source.build.activityCount;
    output.builtScenarioCount = source.build.builtScenarioCount;
    output.reusedScenarioCount = source.build.reusedScenarioCount;
    output.luaFileCount = source.luaFiles;
    output.packBytes = source.packBytes;
    std::memcpy(output.payloadSha256, source.payloadSha256.data(), source.payloadSha256.size());
}

/** Copies one fingerprint. @return False when the size is wrong or every byte is zero. */
[[nodiscard]] bool read_fingerprint(const std::uint8_t* bytes,
                                    std::uint32_t size,
                                    generated::Digest& output) noexcept {
    output = {};
    if (bytes == nullptr || size != output.size()) {
        return false;
    }
    std::memcpy(output.data(), bytes, output.size());
    return std::any_of(
        output.begin(), output.end(), [](std::byte value) { return value != std::byte{}; });
}

/** Resolves a null-terminated absolute path without borrowing a non-terminated view. */
[[nodiscard]] bool full_path(const wchar_t* input, std::wstring& output) noexcept {
    output.clear();
    if (input == nullptr || input[0] == L'\0') {
        return false;
    }
    const DWORD needed = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (needed == 0) {
        return false;
    }
    try {
        std::wstring pending(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(input, needed, pending.data(), nullptr);
        if (written == 0 || written >= needed) {
            return false;
        }
        pending.resize(written);
        while (pending.size() > 3U && (pending.back() == L'\\' || pending.back() == L'/')) {
            pending.pop_back();
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Compares canonical Windows paths without locale-sensitive folding. */
[[nodiscard]] bool same_path(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size()
           && CompareStringOrdinal(left.data(),
                                   static_cast<int>(left.size()),
                                   right.data(),
                                   static_cast<int>(right.size()),
                                   TRUE)
                  == CSTR_EQUAL;
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

/** Creates or accepts the one dedicated generated-estate parent below Sunrise artifacts. */
[[nodiscard]] bool ensure_estate_root(const std::wstring& artifact, std::wstring& output) noexcept {
    if (!ordinary_ancestry(artifact)) {
        output.clear();
        return false;
    }
    try {
        output = artifact;
        output.append(L"\\activity_sdk_estates");
    } catch (...) {
        output.clear();
        return false;
    }
    if (CreateDirectoryW(output.c_str(), nullptr) == FALSE
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD artifactAttributes = GetFileAttributesW(artifact.c_str());
    const DWORD estateAttributes = GetFileAttributesW(output.c_str());
    return ordinary_ancestry(output) && artifactAttributes != INVALID_FILE_ATTRIBUTES
           && (artifactAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
           && estateAttributes != INVALID_FILE_ATTRIBUTES
           && (estateAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * Restricts the exported process-local writer to one direct child of its dedicated Sunrise-owned
 * estate directory. Internal synthetic helpers retain arbitrary isolated output roots.
 */
[[nodiscard]] bool exported_output_allowed(const wchar_t* requested) noexcept {
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&exported_output_allowed),
                           &module)
            == FALSE
        || module == nullptr) {
        return false;
    }
    core::path::Buffer artifactBuffer{};
    if (!core::path::artifact_directory(module, artifactBuffer)) {
        return false;
    }
    try {
        const std::wstring artifact(artifactBuffer.chars.data(), artifactBuffer.length);
        std::wstring estateRoot;
        std::wstring requestedPath;
        if (!ensure_estate_root(artifact, estateRoot) || !full_path(requested, requestedPath)) {
            return false;
        }
        const std::size_t separator = requestedPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos || separator + 1U >= requestedPath.size()
            || !same_path(std::wstring_view(requestedPath).substr(0, separator), estateRoot)) {
            return false;
        }
        const std::wstring_view leaf = std::wstring_view(requestedPath).substr(separator + 1U);
        if (leaf == L"." || leaf == L"..") {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(requestedPath.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES
               || ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } catch (...) {
        return false;
    }
}

} // namespace
} // namespace sunrise::client::content::activity::sdk_generation::one_shot

/** Runs the off-by-default in-DLL generator using only adopted process-local reader material. */
extern "C" __declspec(dllexport) std::uint32_t SunriseGenerateActivitySdkOneShot(
    const wchar_t* packageDirectory,
    const wchar_t* cacheArtifactDirectory,
    const wchar_t* outputArtifactDirectory,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::CancelProbe cancel,
    void* cancelContext,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::ProgressSink progress,
    void* progressContext,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::Result* output) noexcept {
    namespace one_shot = sunrise::client::content::activity::sdk_generation::one_shot;
    if (output != nullptr) {
        one_shot::copy_result(one_shot::Status::invalidInput, {}, *output);
    }
    if (output == nullptr || packageDirectory == nullptr || outputArtifactDirectory == nullptr
        || !one_shot::exported_output_allowed(outputArtifactDirectory)) {
        return static_cast<std::uint32_t>(one_shot::Status::invalidInput);
    }
    static_cast<void>(cacheArtifactDirectory);
    one_shot::ProgressAdapter adapter{progress, progressContext};
    one_shot::Result result{};
    const one_shot::Status status = one_shot::run_process_local(packageDirectory,
                                                                {},
                                                                outputArtifactDirectory,
                                                                cancel,
                                                                cancelContext,
                                                                &one_shot::forward_progress,
                                                                &adapter,
                                                                result);
    one_shot::copy_result(status, result, *output);
    return static_cast<std::uint32_t>(status);
}

/** Returns the retained result for one estate generated by this process. */
extern "C" __declspec(dllexport) std::uint32_t SunriseVerifyActivitySdkEstate(
    const wchar_t* artifactDirectory,
    const std::uint8_t* sourceFingerprint,
    std::uint32_t sourceFingerprintSize,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::CancelProbe cancel,
    void* cancelContext,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::Result* output) noexcept {
    namespace one_shot = sunrise::client::content::activity::sdk_generation::one_shot;
    if (output != nullptr) {
        one_shot::copy_result(one_shot::Status::invalidInput, {}, *output);
    }
    if (output == nullptr || artifactDirectory == nullptr) {
        return static_cast<std::uint32_t>(one_shot::Status::invalidInput);
    }
    one_shot::generated::Digest fingerprint{};
    one_shot::Result result{};
    const one_shot::Status status =
        one_shot::read_fingerprint(sourceFingerprint, sourceFingerprintSize, fingerprint)
            ? one_shot::accept_generated(
                  artifactDirectory, fingerprint, cancel, cancelContext, result)
            : one_shot::Status::invalidInput;
    one_shot::copy_result(status, result, *output);
    return static_cast<std::uint32_t>(status);
}

/** Publishes one retained sibling estate without reopening generated files. */
extern "C" __declspec(dllexport) std::uint32_t SunrisePublishActivitySdkEstate(
    const wchar_t* stageArtifactDirectory,
    const wchar_t* outputArtifactDirectory,
    const std::uint8_t* sourceFingerprint,
    std::uint32_t sourceFingerprintSize,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::CancelProbe cancel,
    void* cancelContext,
    sunrise::client::content::activity::sdk_generation::one_shot::abi::Result* output) noexcept {
    namespace one_shot = sunrise::client::content::activity::sdk_generation::one_shot;
    if (output != nullptr) {
        one_shot::copy_result(one_shot::Status::invalidInput, {}, *output);
    }
    if (output == nullptr || stageArtifactDirectory == nullptr || outputArtifactDirectory == nullptr
        || !one_shot::exported_output_allowed(stageArtifactDirectory)
        || !one_shot::exported_output_allowed(outputArtifactDirectory)) {
        return static_cast<std::uint32_t>(one_shot::Status::invalidInput);
    }
    one_shot::generated::Digest fingerprint{};
    one_shot::Result result{};
    const one_shot::Status status =
        one_shot::read_fingerprint(sourceFingerprint, sourceFingerprintSize, fingerprint)
            ? one_shot::publish_generated(stageArtifactDirectory,
                                          outputArtifactDirectory,
                                          fingerprint,
                                          cancel,
                                          cancelContext,
                                          result)
            : one_shot::Status::invalidInput;
    one_shot::copy_result(status, result, *output);
    return static_cast<std::uint32_t>(status);
}
