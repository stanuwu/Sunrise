#include <Windows.h>

#include <cstdint>
#include <limits>

#include "../../../../core/filesystem/path.h"
#include "../internal.h"
#include "../records/validation.h"
#include "cache_payload_writer.h"
#include "temporary/temporary_cache_file.h"
#include "validation/cache_file_comparison.h"

namespace sunrise::state::build_data::cache {
namespace {

/** @return True when every row count fits the disk header. */
[[nodiscard]] bool counts_fit_header(records::Domains domains) noexcept {
    /** The header uses unsigned 32-bit row counts for every domain. */
    constexpr std::size_t kMaximumCount = (std::numeric_limits<std::uint32_t>::max)();
    return domains.named.size() <= kMaximumCount && domains.items.size() <= kMaximumCount
           && domains.itemDetails.size() <= kMaximumCount
           && domains.inventoryBuckets.size() <= kMaximumCount
           && domains.socketEntryLists.size() <= kMaximumCount
           && domains.socketEntryTables.size() <= kMaximumCount
           && domains.abilityBuckets.size() <= kMaximumCount
           && domains.progressions.size() <= kMaximumCount
           && domains.scenarios.size() <= kMaximumCount
           && domains.rosterGroups.size() <= kMaximumCount;
}

/** @return True when the requested final-name rule is one of the declared values. */
[[nodiscard]] bool valid_disposition(WriteDisposition disposition) noexcept {
    return disposition == WriteDisposition::createOnly
           || disposition == WriteDisposition::replaceStale;
}

} // namespace

/**
 * Writes a sibling file first, then creates or replaces the whole cache in one step.
 * @param directory Null-terminated cache directory.
 * @param path Null-terminated final cache path.
 * @param build Current executable and configured-equipment identity.
 * @param domains Complete sorted mapping domains.
 * @param disposition Create only, or replace a stale cache.
 * @return True when the file is on disk under its requested final name.
 */
bool write(const wchar_t* directory,
           const wchar_t* path,
           const BuildIdentity& build,
           records::Domains domains,
           WriteDisposition disposition) noexcept {
    if (directory == nullptr || path == nullptr || build.imageSize == 0
        || !valid_disposition(disposition) || !counts_fit_header(domains)
        || !records::valid_domains(domains)) {
        return false;
    }
    if (CreateDirectoryW(directory, nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD existingAttributes = GetFileAttributesW(path);
    if (existingAttributes != INVALID_FILE_ATTRIBUTES) {
        if ((existingAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
    }

    std::uint64_t checksum = 0;
    if (!writer::payload_checksum(domains, checksum)) {
        return false;
    }
    core::path::Buffer temporaryPath;
    if (!temporary::write(path, build, domains, checksum, temporaryPath)) {
        return false;
    }
    DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
    if (disposition == WriteDisposition::replaceStale) {
        moveFlags |= MOVEFILE_REPLACE_EXISTING;
    }
    bool complete = MoveFileExW(temporaryPath.chars.data(), path, moveFlags) != FALSE;
    if (!complete && disposition == WriteDisposition::createOnly) {
        // The loser of the race succeeds only when the winner wrote these exact bytes.
        complete = validation::files_equal(path, temporaryPath.chars.data());
    }
    if (!complete || GetFileAttributesW(temporaryPath.chars.data()) != INVALID_FILE_ATTRIBUTES) {
        (void)DeleteFileW(temporaryPath.chars.data());
    }
    return complete;
}

} // namespace sunrise::state::build_data::cache
