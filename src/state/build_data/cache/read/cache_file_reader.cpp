#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "../internal.h"
#include "cache_payload_reader.h"

namespace sunrise::state::build_data::cache {
namespace {

/** @return True when every required domain is nonempty. */
[[nodiscard]] bool required_domains_present(const records::DomainCounts& counts) noexcept {
    return counts.named != 0 && counts.items != 0 && counts.inventoryBuckets != 0
           && counts.socketEntryLists != 0 && counts.progressions != 0 && counts.scenarios != 0;
}

/** @return True when every count fits the output storage. */
[[nodiscard]] bool counts_fit(const records::DomainCounts& counts,
                              records::MutableDomains output) noexcept {
    return counts.named <= output.named.size() && counts.items <= output.items.size()
           && counts.itemDetails <= output.itemDetails.size()
           && counts.inventoryBuckets <= output.inventoryBuckets.size()
           && counts.socketEntryLists <= output.socketEntryLists.size()
           && counts.socketEntryTables <= output.socketEntryTables.size()
           && counts.abilityBuckets <= output.abilityBuckets.size()
           && counts.progressions <= output.progressions.size()
           && counts.scenarios <= output.scenarios.size()
           && counts.rosterGroups <= output.rosterGroups.size()
           && counts.spawnStems <= output.spawnStems.size()
           && counts.spawnNameHashes <= output.spawnNameHashes.size()
           && counts.hashNames <= output.hashNames.size();
}

/** @return The header's row counts, as platform sizes. */
[[nodiscard]] records::DomainCounts counts_of(const records::Header& header) noexcept {
    return {
        header.namedCount,
        header.itemCount,
        header.itemDetailCount,
        header.inventoryBucketCount,
        header.socketEntryListCount,
        header.socketEntryTableCount,
        header.abilityBucketCount,
        header.progressionCount,
        header.scenarioCount,
        header.rosterGroupCount,
        header.spawnStemCount,
        header.spawnNameHashCount,
        header.hashNameCount,
    };
}

/**
 * Every format below the current one is out of date, so a version bump needs no edit here.
 * Listing them one by one left a bumped version unknown, and a valid old cache read as corrupt.
 * @param version Cache prefix version.
 * @return True when the cache is older than the current format.
 */
[[nodiscard]] bool stale_format(std::uint32_t version) noexcept {
    return version < records::kCacheFormatVersion;
}

/** @return The pending status, or invalid when the file fails to close. */
[[nodiscard]] LoadStatus close_with(HANDLE file, LoadStatus status) noexcept {
    return CloseHandle(file) != FALSE ? status : LoadStatus::invalid;
}

} // namespace

/** Reads the PE identity and ties it to the configured-equipment hash. */
bool current_build_identity(std::uint64_t configuredEquipmentHash,
                            BuildIdentity& identity) noexcept {
    identity = {};
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return false;
    }
    const auto* image = reinterpret_cast<const std::byte*>(module);
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof dos);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, image + dos.e_lfanew, sizeof nt);
    if (nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.OptionalHeader.SizeOfImage == 0) {
        return false;
    }
    identity.imageTimestamp = nt.FileHeader.TimeDateStamp;
    identity.imageSize = nt.OptionalHeader.SizeOfImage;
    identity.configuredEquipmentHash = configuredEquipmentHash;
    return true;
}

/** Loads every build-bound domain and commits the counts only after the file closes. */
LoadStatus load(const wchar_t* path,
                const BuildIdentity& expectedBuild,
                records::MutableDomains output,
                records::DomainCounts& counts) noexcept {
    counts = {};
    read::clear(output);
    if (path == nullptr || expectedBuild.imageSize == 0 || output.constants == nullptr) {
        return LoadStatus::invalid;
    }
    const HANDLE file = CreateFileW(path,
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? LoadStatus::missing
                                                                              : LoadStatus::invalid;
    }

    LARGE_INTEGER actualSize{};
    records::Prefix prefix{};
    if (GetFileSizeEx(file, &actualSize) == FALSE || actualSize.QuadPart <= 0
        || !read::read_value(file, prefix) || prefix.magic != records::kCacheMagic) {
        return close_with(file, LoadStatus::invalid);
    }
    if (stale_format(prefix.version)) {
        return close_with(file, LoadStatus::stale);
    }
    if (prefix.version != records::kCacheFormatVersion) {
        return close_with(file, LoadStatus::invalid);
    }

    LARGE_INTEGER beginning{};
    records::Header header{};
    if (SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) == FALSE
        || !read::read_value(file, header) || header.magic != records::kCacheMagic
        || header.version != records::kCacheFormatVersion) {
        return close_with(file, LoadStatus::invalid);
    }
    const BuildIdentity cachedBuild{
        header.imageTimestamp,
        header.imageSize,
        header.configuredEquipmentHash,
    };
    if (!(cachedBuild == expectedBuild)) {
        return close_with(file, LoadStatus::stale);
    }

    const records::DomainCounts pendingCounts = counts_of(header);
    std::uint64_t expectedSize = 0;
    std::uint64_t checksum = 0;
    bool valid = required_domains_present(pendingCounts) && counts_fit(pendingCounts, output)
                 && read::expected_size(pendingCounts, expectedSize)
                 && static_cast<std::uint64_t>(actualSize.QuadPart) == expectedSize
                 && read::read_payload(file, header.constants, pendingCounts, output, checksum)
                 && checksum == header.payloadChecksum;
    const LoadStatus status = close_with(file, valid ? LoadStatus::loaded : LoadStatus::invalid);
    if (status != LoadStatus::loaded) {
        // Counts and rows commit together only after the file handle closes cleanly.
        read::clear(output);
        return status;
    }
    counts = pendingCounts;
    // Header scalars commit with the counts, on the same clean-close path as the record arrays.
    *output.constants = header.constants;
    return LoadStatus::loaded;
}

} // namespace sunrise::state::build_data::cache
