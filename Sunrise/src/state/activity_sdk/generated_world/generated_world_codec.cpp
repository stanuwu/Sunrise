#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../middleware/crypto/sha256.h"
#include "codec.h"
#include "format.h"
#include "internal.h"

namespace sunrise::state::activity_sdk::generated_world {
namespace {

namespace catalog = build_data::scriptables;

/** One dot separates the process, thread, and sequence parts of a writer-owned sibling. */
constexpr std::wstring_view kComponentSeparator = L".";
/** Incomplete writer-owned siblings carry the shared cleanup extension. */
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** One Windows identifier is eight uppercase hexadecimal characters. */
constexpr std::size_t kIdentifierDigits = sizeof(DWORD) * 2;
/** Four bits select one hexadecimal digit. */
constexpr std::size_t kBitsPerDigit = 4;
/** The low four bits select one hexadecimal digit. */
constexpr DWORD kDigitMask = (1UL << kBitsPerDigit) - 1UL;
/** Uppercase paths match the shared stale-sibling parser. */
constexpr std::wstring_view kDigits = L"0123456789ABCDEF";
/** Sixteen names bound collision handling without sharing a temporary path. */
constexpr std::size_t kTemporaryAttempts = 16;

volatile LONG g_temporarySequence{};

/** Small non-copying Windows file owner for exception-safe codec paths. */
class FileHandle final {
public:
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    /** Closes the owned handle once and reports the Windows result. */
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

/** Removes a completed temporary file unless its atomic rename succeeds. */
class TemporaryCleanup final {
public:
    explicit TemporaryCleanup(const wchar_t* path) noexcept : path_(path) {}
    TemporaryCleanup(const TemporaryCleanup&) = delete;
    TemporaryCleanup& operator=(const TemporaryCleanup&) = delete;

    ~TemporaryCleanup() {
        if (active_ && path_ != nullptr) {
            (void)DeleteFileW(path_);
        }
    }

    void dismiss() noexcept {
        active_ = false;
    }

private:
    const wchar_t* path_{};
    bool active_{true};
};

/** Reads one exact byte range, refusing a short successful Windows read. */
[[nodiscard]] bool read_exact(HANDLE file, void* output, std::size_t size) noexcept {
    if (size > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return false;
    }
    DWORD transferred = 0;
    return ReadFile(file, output, static_cast<DWORD>(size), &transferred, nullptr) != FALSE
           && static_cast<std::size_t>(transferred) == size;
}

/** Writes one exact byte range, refusing a short successful Windows write. */
[[nodiscard]] bool write_exact(HANDLE file, const void* input, std::size_t size) noexcept {
    if (size > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return false;
    }
    DWORD transferred = 0;
    return WriteFile(file, input, static_cast<DWORD>(size), &transferred, nullptr) != FALSE
           && static_cast<std::size_t>(transferred) == size;
}

/** Opens one shard for a bounded header read or complete load. */
[[nodiscard]] HANDLE
open_read(const wchar_t* path, std::uint32_t expectedScenarioTag, LoadStatus& status) noexcept {
    status = LoadStatus::invalid;
    if (path == nullptr || path[0] == L'\0' || expectedScenarioTag == 0) {
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE rawFile = CreateFileW(path,
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                     ? LoadStatus::missing
                     : LoadStatus::invalid;
    }
    return rawFile;
}

/**
 * Checks the file extent and every identity shared by header checks and full loads.
 * @param file Read handle positioned at the file start.
 * @param expectedScenarioTag Scenario required by the manifest.
 * @param expectedSourceFingerprint Installed content identity.
 * @param header Receives the validated header on success.
 * @param status Receives the precise refusal on failure.
 * @return True when the file can hold a current-format shard of that identity.
 */
[[nodiscard]] bool read_header(HANDLE file,
                               std::uint32_t expectedScenarioTag,
                               const Digest& expectedSourceFingerprint,
                               format::Header& header,
                               LoadStatus& status) noexcept {
    status = LoadStatus::invalid;
    LARGE_INTEGER fileSize{};
    const bool complete = GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart >= 0
                          && read_exact(file, &header, sizeof header);
    if (!complete || header.magic != format::kMagic) {
        return false;
    }
    if (header.version != format::kVersion) {
        status = LoadStatus::versionMismatch;
        return false;
    }
    if (!internal::valid_shape(header, static_cast<std::uint64_t>(fileSize.QuadPart))) {
        return false;
    }
    if (header.scenarioTag != expectedScenarioTag) {
        status = LoadStatus::scenarioMismatch;
        return false;
    }
    if (header.sourceFingerprint != expectedSourceFingerprint) {
        status = LoadStatus::sourceMismatch;
        return false;
    }
    return true;
}

/** Loads one file after the public boundary has established exception handling. */
[[nodiscard]] bool load_file(const wchar_t* path,
                             std::uint32_t expectedScenarioTag,
                             const Digest& expectedSourceFingerprint,
                             catalog::Snapshot& snapshot,
                             Digest& payloadSha256,
                             LoadStatus& status) {
    FileHandle file(open_read(path, expectedScenarioTag, status));
    format::Header header{};
    if (file.get() == INVALID_HANDLE_VALUE
        || !read_header(
            file.get(), expectedScenarioTag, expectedSourceFingerprint, header, status)) {
        return false;
    }
    const std::size_t payloadSize = static_cast<std::size_t>(header.fileSize - header.headerSize);
    std::vector<std::byte> payload(payloadSize);
    bool complete = read_exact(file.get(), payload.data(), payload.size());
    complete = file.close() && complete;
    Digest digest{};
    if (!complete || !middleware::crypto::sha256::hash(std::span<const std::byte>(payload), digest)
        || digest != header.payloadSha256) {
        return false;
    }
    catalog::Snapshot pending{};
    if (!internal::decode_payload(header, payload, pending)) {
        return false;
    }
    snapshot = std::move(pending);
    payloadSha256 = digest;
    status = LoadStatus::loaded;
    return true;
}

/** Appends one fixed-width writer identifier to a temporary path. */
[[nodiscard]] bool append_identifier(core::path::Buffer& path, DWORD value) noexcept {
    if (!core::path::append(path, kComponentSeparator)
        || path.length + kIdentifierDigits >= path.chars.size()) {
        return false;
    }
    for (std::size_t digit = 0; digit < kIdentifierDigits; ++digit) {
        const std::size_t remaining = kIdentifierDigits - digit - 1;
        const std::size_t shift = remaining * kBitsPerDigit;
        const DWORD index = (value >> shift) & kDigitMask;
        path.chars[path.length++] = kDigits[static_cast<std::size_t>(index)];
    }
    path.chars[path.length] = L'\0';
    return true;
}

/** Builds one writer-owned sibling candidate understood by stale-file cleanup. */
[[nodiscard]] bool make_temporary_path(const wchar_t* finalPath,
                                       core::path::Buffer& output) noexcept {
    const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_temporarySequence));
    return core::path::assign(output, finalPath) && append_identifier(output, GetCurrentProcessId())
           && append_identifier(output, GetCurrentThreadId()) && append_identifier(output, sequence)
           && core::path::append(output, kTemporarySuffix);
}

/** Writes and closes one complete file under a newly created sibling name. */
[[nodiscard]] bool write_temporary(const wchar_t* finalPath,
                                   std::span<const std::byte> header,
                                   std::span<const std::byte> payload,
                                   core::path::Buffer& temporaryPath) noexcept {
    core::path::remove_stale_siblings(finalPath);
    for (std::size_t attempt = 0; attempt < kTemporaryAttempts; ++attempt) {
        if (!make_temporary_path(finalPath, temporaryPath)) {
            return false;
        }
        const HANDLE rawFile = CreateFileW(temporaryPath.chars.data(),
                                           GENERIC_WRITE,
                                           0,
                                           nullptr,
                                           CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                           nullptr);
        if (rawFile == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            return false;
        }
        FileHandle file(rawFile);
        bool complete = write_exact(file.get(), header.data(), header.size())
                        && write_exact(file.get(), payload.data(), payload.size());
        complete = file.close() && complete;
        if (!complete) {
            (void)DeleteFileW(temporaryPath.chars.data());
        }
        return complete;
    }
    return false;
}

} // namespace

/** Serializes and hashes one complete deterministic shard image exactly once. */
bool prepare(const Digest& sourceFingerprint,
             const catalog::Snapshot& snapshot,
             PreparedShard& output) noexcept {
    try {
        format::Header header{};
        std::vector<std::byte> payload{};
        if (!internal::build_payload(sourceFingerprint, snapshot, header, payload)) {
            return false;
        }
        PreparedShard pending{};
        pending.header_.resize(sizeof header);
        std::memcpy(pending.header_.data(), &header, sizeof header);
        pending.payload_ = std::move(payload);
        pending.payloadSha256_ = header.payloadSha256;
        output = std::move(pending);
        return true;
    } catch (...) {
        return false;
    }
}

/** Publishes one prepared shard through a closed writer-owned sibling. */
bool publish(const wchar_t* path, PreparedShard prepared, Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    if (path == nullptr || path[0] == L'\0' || prepared.header_.size() != sizeof(format::Header)) {
        return false;
    }
    core::path::Buffer temporaryPath;
    if (!write_temporary(path, prepared.header_, prepared.payload_, temporaryPath)) {
        return false;
    }
    TemporaryCleanup cleanup(temporaryPath.chars.data());
    if (!core::path::publish_sibling(temporaryPath.chars.data(), path)) {
        return false;
    }
    cleanup.dismiss();
    payloadSha256 = prepared.payloadSha256_;
    return true;
}

/** Computes the payload identity while retaining no prepared image. */
bool payload_sha256(const Digest& sourceFingerprint,
                    const catalog::Snapshot& snapshot,
                    Digest& output) noexcept {
    output = {};
    PreparedShard prepared{};
    if (!prepare(sourceFingerprint, snapshot, prepared)) {
        return false;
    }
    output = prepared.payload_sha256();
    return true;
}

/** Writes one deterministic scenario snapshot through a closed writer-owned sibling. */
bool write(const wchar_t* path,
           const Digest& sourceFingerprint,
           const catalog::Snapshot& snapshot,
           Digest& payloadSha256) noexcept {
    payloadSha256 = {};
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    PreparedShard prepared{};
    return prepare(sourceFingerprint, snapshot, prepared)
           && publish(path, std::move(prepared), payloadSha256);
}

/**
 * Checks the manifest-owned header before startup reuses a published estate.
 * @param path Exact shard file.
 * @param expectedScenarioTag Manifest scenario identity.
 * @param expectedSourceFingerprint Installed content identity.
 * @param expectedPayloadSha256 Digest declared by the authenticated manifest.
 * @return True for a current header; full load still authenticates the payload.
 */
bool compatible_header(const wchar_t* path,
                       std::uint32_t expectedScenarioTag,
                       const Digest& expectedSourceFingerprint,
                       const Digest& expectedPayloadSha256) noexcept {
    LoadStatus status = LoadStatus::invalid;
    FileHandle file(open_read(path, expectedScenarioTag, status));
    format::Header header{};
    return file.get() != INVALID_HANDLE_VALUE
           && read_header(
               file.get(), expectedScenarioTag, expectedSourceFingerprint, header, status)
           && header.payloadSha256 == expectedPayloadSha256;
}

/** Loads and validates one scenario shard without partially replacing the caller's snapshot. */
bool load(const wchar_t* path,
          std::uint32_t expectedScenarioTag,
          const Digest& expectedSourceFingerprint,
          catalog::Snapshot& snapshot,
          Digest& payloadSha256,
          LoadStatus& status) noexcept {
    payloadSha256 = {};
    try {
        return load_file(
            path, expectedScenarioTag, expectedSourceFingerprint, snapshot, payloadSha256, status);
    } catch (...) {
        payloadSha256 = {};
        status = LoadStatus::invalid;
        return false;
    }
}

} // namespace sunrise::state::activity_sdk::generated_world
