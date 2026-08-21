#include "temporary_cache_file.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../../../core/filesystem/temporary_sibling.h"
#include "../../records/format.h"
#include "../cache_payload_writer.h"

namespace sunrise::state::build_data::cache::temporary {
namespace {

/** One dot separates the writer id parts from the final cache name. */
constexpr std::wstring_view kComponentSeparator = L".";
/** A temporary extension marks an unfinished cache file. */
constexpr std::wstring_view kSuffix = L".tmp";
/** One byte is 2 hex characters in the path. */
constexpr std::size_t kHexadecimalDigitsPerByte = 2;
/** A Windows DWORD writer id is 8 hex characters in the path. */
constexpr std::size_t kWriterIdentifierDigits = sizeof(DWORD) * kHexadecimalDigitsPerByte;
/** 4 bits pick one hex digit. */
constexpr std::size_t kBitsPerHexadecimalDigit = 4;
/** The low 4 bits pick one character from the hex digit table. */
constexpr DWORD kHexadecimalDigitMask = (1UL << kBitsPerHexadecimalDigit) - 1UL;
/** Uppercase hex keeps the process, thread, and sequence parts path-safe. */
constexpr std::wstring_view kHexadecimalDigits = L"0123456789ABCDEF";
/** 16 tries cover stale same-process temporary names without retrying forever. */
constexpr std::size_t kCreateAttemptCapacity = 16;

/** Process-local sequence separates concurrent writers running on one thread. */
volatile LONG g_sequence = 0;

/** Result of one attempt to create the temporary file. */
enum class WriteStatus {
    complete,
    collision,
    failed,
};

/**
 * Appends one fixed-width hex writer id to a temporary path.
 * @param path Mutable null-terminated path.
 * @param value Windows process, thread, or sequence id.
 * @return True when the separator, the digits, and the null all fit.
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
 * Builds one writer-owned sibling name from the process, thread, and sequence ids.
 * @param finalPath Null-terminated final cache path.
 * @param temporaryPath Receives a unique candidate sibling path.
 * @return True when every part fits.
 */
[[nodiscard]] bool make_path(const wchar_t* finalPath, core::path::Buffer& temporaryPath) noexcept {
    const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_sequence));
    return core::path::assign(temporaryPath, finalPath)
           && append_identifier(temporaryPath, GetCurrentProcessId())
           && append_identifier(temporaryPath, GetCurrentThreadId())
           && append_identifier(temporaryPath, sequence)
           && core::path::append(temporaryPath, kSuffix);
}

/**
 * Creates and flushes one sibling temporary file without touching the final name.
 * @param temporaryPath Null-terminated sibling temporary path.
 * @param build Complete build identity.
 * @param domains Complete sorted mapping domains.
 * @param checksum Payload checksum computed earlier, stored in the header.
 * @return Complete, a name collision, or another write failure.
 */
[[nodiscard]] WriteStatus write_file(const wchar_t* temporaryPath,
                                     const BuildIdentity& build,
                                     records::Domains domains,
                                     std::uint64_t checksum) noexcept {
    const HANDLE file = CreateFileW(temporaryPath,
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? WriteStatus::collision
                                                                           : WriteStatus::failed;
    }
    const records::Header header{
        records::kCacheMagic,
        records::kCacheFormatVersion,
        build.imageTimestamp,
        build.imageSize,
        build.configuredEquipmentHash,
        static_cast<std::uint32_t>(domains.named.size()),
        static_cast<std::uint32_t>(domains.items.size()),
        static_cast<std::uint32_t>(domains.collectibles.size()),
        static_cast<std::uint32_t>(domains.materialRequirementSets.size()),
        static_cast<std::uint32_t>(domains.itemDetails.size()),
        static_cast<std::uint32_t>(domains.socketPlugRules.size()),
        static_cast<std::uint32_t>(domains.socketPlugPools.size()),
        static_cast<std::uint32_t>(domains.socketPlugMembers.size()),
        static_cast<std::uint32_t>(domains.exoticCatalysts.size()),
        static_cast<std::uint32_t>(domains.inventoryBuckets.size()),
        static_cast<std::uint32_t>(domains.socketEntryLists.size()),
        static_cast<std::uint32_t>(domains.socketEntryTables.size()),
        static_cast<std::uint32_t>(domains.abilityBuckets.size()),
        static_cast<std::uint32_t>(domains.progressions.size()),
        static_cast<std::uint32_t>(domains.scenarios.size()),
        static_cast<std::uint32_t>(domains.rosterGroups.size()),
        static_cast<std::uint32_t>(domains.spawnStems.size()),
        static_cast<std::uint32_t>(domains.spawnNameHashes.size()),
        static_cast<std::uint32_t>(domains.spawnPoints.size()),
        static_cast<std::uint32_t>(domains.hashNames.size()),
        static_cast<std::uint32_t>(domains.vendorIndex.size()),
        static_cast<std::uint32_t>(domains.vendorDefinitions.size()),
        static_cast<std::uint32_t>(domains.vendorSaleRows.size()),
        static_cast<std::uint32_t>(domains.vendorInstalledRows.size()),
        domains.constants,
        checksum,
    };
    DWORD transferred = 0;
    bool complete = WriteFile(file, &header, sizeof header, &transferred, nullptr) != FALSE
                    && transferred == sizeof header && writer::write_payload(file, domains)
                    && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        // This writer owns a CREATE_NEW path, so a failed partial file is safe to remove.
        (void)DeleteFileW(temporaryPath);
    }
    return complete ? WriteStatus::complete : WriteStatus::failed;
}

} // namespace

/** Writes one whole cache under a new sibling name only this writer owns. */
bool write(const wchar_t* finalPath,
           const BuildIdentity& build,
           records::Domains domains,
           std::uint64_t checksum,
           core::path::Buffer& temporaryPath) noexcept {
    core::path::remove_stale_siblings(finalPath);
    for (std::size_t attempt = 0; attempt < kCreateAttemptCapacity; ++attempt) {
        if (!make_path(finalPath, temporaryPath)) {
            return false;
        }
        const WriteStatus status = write_file(temporaryPath.chars.data(), build, domains, checksum);
        if (status == WriteStatus::complete) {
            return true;
        }
        if (status == WriteStatus::failed) {
            return false;
        }
        // Only a name collision moves on to the next process-local sequence number.
    }
    return false;
}

} // namespace sunrise::state::build_data::cache::temporary
