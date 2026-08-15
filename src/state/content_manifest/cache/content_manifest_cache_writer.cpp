#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/filesystem/temporary_sibling.h"
#include "../content_manifest_row_validation.h"
#include "format.h"
#include "internal.h"

namespace sunrise::state::content_manifest::cache {
namespace {

/** 8 unique-name tries bound one all-or-nothing cache publish. */
constexpr std::size_t kTemporaryNameAttempts = 8;
/** One dot separates each writer id part from the final cache name. */
constexpr std::wstring_view kComponentSeparator = L".";
/** A temporary extension marks incomplete cache files, to help diagnosis. */
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** One byte takes 2 hex path characters. */
constexpr std::size_t kHexadecimalDigitsPerByte = 2;
/** A Windows DWORD writer id takes 8 hex path characters. */
constexpr std::size_t kWriterIdentifierDigits = sizeof(DWORD) * kHexadecimalDigitsPerByte;
/** 4 bits pick one hex digit. */
constexpr std::size_t kBitsPerHexadecimalDigit = 4;
/** The low 4 bits pick one character from the hex digit table. */
constexpr DWORD kHexadecimalDigitMask = (1UL << kBitsPerHexadecimalDigit) - 1UL;
/** Uppercase hex keeps the process, thread and sequence parts path-safe. */
constexpr std::wstring_view kHexadecimalDigits = L"0123456789ABCDEF";

/** Process-local sequence separates concurrent writers running on one thread. */
volatile LONG g_temporaryCounter{};

/**
 * Writes one fixed object, and refuses a short but successful Windows write.
 * @tparam Value Trivially copied cache object.
 * @param file Open temporary cache handle at the required sequential offset.
 * @param value Complete canonical object bytes.
 * @return True when every object byte is written.
 */
template <typename Value> [[nodiscard]] bool write_exact(HANDLE file, const Value& value) noexcept {
    DWORD copied = 0;
    return sizeof value <= (std::numeric_limits<DWORD>::max)()
           && WriteFile(file, &value, static_cast<DWORD>(sizeof value), &copied, nullptr) != FALSE
           && copied == sizeof value;
}

/** @param row Canonical State row. @param output Cleared stable disk row. */
void encode_row(const Row& row, DiskRow& output) noexcept {
    output = {};
    output.name = row.name;
    output.nameLength = row.nameLength;
    output.packageId = row.packageId;
    output.buildSignature = row.buildSignature;
}

/**
 * Appends one fixed-width hex writer id to a temporary path.
 * @param path Mutable null-terminated path.
 * @param value Windows process, thread, or sequence id.
 * @return True when the separator, digits and null fit fixed storage.
 */
[[nodiscard]] bool append_identifier(core::path::Buffer& path, DWORD value) noexcept {
    if (!core::path::append(path, kComponentSeparator)
        || path.length + kWriterIdentifierDigits >= path.chars.size()) {
        return false;
    }
    for (std::size_t digit = 0; digit < kWriterIdentifierDigits; ++digit) {
        const std::size_t remainingDigits = kWriterIdentifierDigits - digit - 1U;
        const std::size_t shift = remainingDigits * kBitsPerHexadecimalDigit;
        const std::size_t character = (value >> shift) & kHexadecimalDigitMask;
        path.chars[path.length++] = kHexadecimalDigits[character];
    }
    path.chars[path.length] = L'\0';
    return true;
}

/**
 * Builds one writer-owned sibling name without using shared or random storage.
 * @param finalPath Complete final cache path.
 * @param counter Rising process-local name part.
 * @param output Cleared fixed temporary path.
 * @return True when the complete sibling name fits.
 */
[[nodiscard]] bool
make_temporary_path(const wchar_t* finalPath, LONG counter, core::path::Buffer& output) noexcept {
    return finalPath != nullptr && core::path::assign(output, finalPath)
           && append_identifier(output, GetCurrentProcessId())
           && append_identifier(output, GetCurrentThreadId())
           && append_identifier(output, static_cast<DWORD>(counter))
           && core::path::append(output, kTemporarySuffix);
}

/**
 * Opens one unique sibling for an all-or-nothing cache write.
 * @param finalPath Complete final cache path.
 * @param temporaryPath Receives the exact created sibling path.
 * @return Writable create-new handle or INVALID_HANDLE_VALUE.
 */
[[nodiscard]] HANDLE open_temporary(const wchar_t* finalPath,
                                    core::path::Buffer& temporaryPath) noexcept {
    for (std::size_t attempt = 0; attempt < kTemporaryNameAttempts; ++attempt) {
        const LONG counter = InterlockedIncrement(&g_temporaryCounter);
        if (!make_temporary_path(finalPath, counter, temporaryPath)) {
            return INVALID_HANDLE_VALUE;
        }
        const HANDLE file = CreateFileW(temporaryPath.chars.data(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) {
            return file;
        }
    }
    return INVALID_HANDLE_VALUE;
}

/** @param directory Owned cache directory. @return True for a created or existing directory. */
[[nodiscard]] bool ensure_directory(const wchar_t* directory) noexcept {
    if (directory == nullptr) {
        return false;
    }
    if (CreateDirectoryW(directory, nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * Compares the reopened temporary cache with the source catalog.
 * @param path Temporary cache path.
 * @param directoryFingerprint Current recognized-file identity.
 * @param buildFingerprint Expected selected-row identity.
 * @param rows Source canonical rows.
 * @param validationRows Fixed reopened-row scratch.
 * @return True when the load check reproduces every public field.
 */
[[nodiscard]] bool validate_temporary(const wchar_t* path,
                                      const Fingerprint& directoryFingerprint,
                                      const Fingerprint& buildFingerprint,
                                      std::span<const Row> rows,
                                      std::span<Row> validationRows) noexcept {
    std::size_t count = 0;
    Fingerprint loadedFingerprint{};
    Guid loadedGuid{};
    const LoadStatus status =
        load(path, directoryFingerprint, validationRows, count, loadedFingerprint, loadedGuid);
    return status == LoadStatus::loaded && count == rows.size()
           && loadedFingerprint == buildFingerprint
           && std::equal(rows.begin(),
                         rows.end(),
                         validationRows.begin(),
                         [](const Row& left, const Row& right) {
                             return left.name == right.name && left.nameLength == right.nameLength
                                    && left.packageId == right.packageId
                                    && left.buildSignature == right.buildSignature;
                         });
}

} // namespace

/** Writes, reopens, checks, then publishes one generated cache with one rename. */
bool write(const wchar_t* directory,
           const wchar_t* path,
           const Fingerprint& directoryFingerprint,
           const Fingerprint& buildFingerprint,
           std::span<const Row> rows,
           std::span<Row> validationRows) noexcept {
    if (!valid(rows) || validationRows.size() < kRowCapacity || !ensure_directory(directory)) {
        return false;
    }
    core::path::remove_stale_siblings(path);
    Header header{};
    header.magic = kCacheMagic;
    header.version = kCacheVersion;
    header.headerSize = sizeof(Header);
    header.rowSize = sizeof(DiskRow);
    header.rowCount = static_cast<std::uint32_t>(rows.size());
    header.directoryFingerprint = directoryFingerprint;
    header.buildFingerprint = buildFingerprint;

    core::path::Buffer temporaryPath;
    const HANDLE file = open_temporary(path, temporaryPath);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool complete = write_exact(file, header);
    for (const Row& row : rows) {
        DiskRow diskRow{};
        encode_row(row, diskRow);
        if (!complete || !write_exact(file, diskRow)) {
            complete = false;
            break;
        }
    }
    complete = complete && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    complete = complete
               && validate_temporary(temporaryPath.chars.data(),
                                     directoryFingerprint,
                                     buildFingerprint,
                                     rows,
                                     validationRows);
    if (complete) {
        // The final name changes only after the sibling reopens as a complete valid cache.
        complete = MoveFileExW(temporaryPath.chars.data(),
                               path,
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   != FALSE;
    }
    if (!complete) {
        DeleteFileW(temporaryPath.chars.data());
    }
    return complete;
}

} // namespace sunrise::state::content_manifest::cache
