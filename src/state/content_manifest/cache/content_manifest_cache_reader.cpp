#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../content_manifest_row_validation.h"
#include "../fingerprint/content_manifest_fingerprint.h"
#include "format.h"
#include "internal.h"

namespace sunrise::state::content_manifest::cache {
namespace {

/**
 * Reads one fixed object, and refuses a short but successful Windows read.
 * @tparam Value Trivially copied cache object.
 * @param file Open cache handle at the required sequential offset.
 * @param output Cleared destination object.
 * @return True when every object byte is read.
 */
template <typename Value> [[nodiscard]] bool read_exact(HANDLE file, Value& output) noexcept {
    output = {};
    DWORD copied = 0;
    return sizeof output <= (std::numeric_limits<DWORD>::max)()
           && ReadFile(file, &output, static_cast<DWORD>(sizeof output), &copied, nullptr) != FALSE
           && copied == sizeof output;
}

/** @param row Stable disk row. @param output Cleared native State row. */
void decode_row(const DiskRow& row, Row& output) noexcept {
    output = {};
    output.name = row.name;
    output.nameLength = row.nameLength;
    output.packageId = row.packageId;
    output.buildSignature = row.buildSignature;
}

/**
 * Works out the one byte length allowed for a declared row count.
 * @param rowCount Bounded cache row count.
 * @param size Receives the whole cache byte length.
 * @return True when the multiply and add fit a signed Windows file size.
 */
[[nodiscard]] bool expected_size(std::uint32_t rowCount, std::uint64_t& size) noexcept {
    size = 0;
    if (rowCount == 0 || rowCount > kRowCapacity) {
        return false;
    }
    const std::uint64_t payload = static_cast<std::uint64_t>(rowCount) * sizeof(DiskRow);
    if (payload
        > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) - sizeof(Header)) {
        return false;
    }
    size = sizeof(Header) + payload;
    return true;
}

/** @param rows Output storage. @param count Used prefix length. */
void clear_rows(std::span<Row> rows, std::size_t& count) noexcept {
    count = 0;
    if (!rows.empty()) {
        SecureZeroMemory(rows.data(), rows.size_bytes());
    }
}

} // namespace

/** Loads and checks one exact cache into fixed caller storage. */
LoadStatus load(const wchar_t* path,
                const Fingerprint& directoryFingerprint,
                std::span<Row> rows,
                std::size_t& count,
                Fingerprint& buildFingerprint,
                Guid& guidValue) noexcept {
    clear_rows(rows, count);
    buildFingerprint = {};
    guidValue = {};
    if (path == nullptr || rows.size() < kRowCapacity) {
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

    Header header{};
    LARGE_INTEGER fileSize{};
    std::uint64_t exactSize = 0;
    bool complete = read_exact(file, header) && GetFileSizeEx(file, &fileSize) != FALSE
                    && fileSize.QuadPart >= 0 && header.magic == kCacheMagic
                    && header.version == kCacheVersion && header.headerSize == sizeof(Header)
                    && header.rowSize == sizeof(DiskRow) && header.reserved == 0
                    && expected_size(header.rowCount, exactSize)
                    && static_cast<std::uint64_t>(fileSize.QuadPart) == exactSize;
    if (!complete) {
        CloseHandle(file);
        return LoadStatus::invalid;
    }
    if (header.directoryFingerprint != directoryFingerprint) {
        CloseHandle(file);
        return LoadStatus::stale;
    }

    for (std::uint32_t index = 0; index < header.rowCount; ++index) {
        DiskRow diskRow{};
        if (!read_exact(file, diskRow)) {
            complete = false;
            break;
        }
        decode_row(diskRow, rows[index]);
    }
    CloseHandle(file);
    if (!complete) {
        clear_rows(rows, count);
        return LoadStatus::invalid;
    }

    count = header.rowCount;
    const std::span<const Row> occupied = rows.first(count);
    Fingerprint computed{};
    if (!valid(occupied) || !fingerprint::rows(occupied, computed)
        || computed != header.buildFingerprint) {
        clear_rows(rows, count);
        return LoadStatus::invalid;
    }
    buildFingerprint = computed;
    fingerprint::guid(buildFingerprint, guidValue);
    return LoadStatus::loaded;
}

} // namespace sunrise::state::content_manifest::cache
