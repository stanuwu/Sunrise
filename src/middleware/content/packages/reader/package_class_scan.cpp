#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

/** Entry indices are 13 bits of a tag handle, so a table cannot be larger than this. */
constexpr std::uint32_t kMaximumEntries = layout::kTagEntryMask + 1U;
/** Entry records are read in fixed batches to keep the table off the caller stack. */
constexpr std::size_t kBatchRecords = 256;
/** One bit per package id records which ids have been scanned. */
constexpr std::size_t kSeenWords = 1024;
/** 64 ids share one word of the seen set. */
constexpr std::uint16_t kSeenWordShift = 6;
/** The low bits of an id pick its bit inside that word. */
constexpr std::uint16_t kSeenBitMask = 63;

using SeenSet = std::array<std::uint64_t, kSeenWords>;

/** @param seen Scanned-id set. @return True when this package id is new. */
[[nodiscard]] bool claim(SeenSet& seen, std::uint16_t packageId) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << (packageId & kSeenBitMask);
    std::uint64_t& word = seen[packageId >> kSeenWordShift];
    if ((word & bit) != 0) {
        return false;
    }
    word |= bit;
    return true;
}

/**
 * Builds the file-search pattern for one installed packages directory.
 * @param directory Installed packages directory, with or without a trailing separator.
 * @param search Receives the whole pattern.
 * @return True when the pattern fits fixed storage.
 */
[[nodiscard]] bool search_pattern(std::wstring_view directory, Path& search) noexcept {
    const bool separated = !directory.empty() && directory.back() == L'\\';
    const int written = std::swprintf(search.chars.data(),
                                      search.chars.size(),
                                      separated ? L"%.*s*.pkg" : L"%.*s\\*.pkg",
                                      static_cast<int>(directory.size()),
                                      directory.data());
    return written > 0;
}

/**
 * Reports the matching entries of one package's highest patch.
 * @param directory Installed packages directory.
 * @param packageId Package id to scan.
 * @param classId Tag class to report.
 * @param visitor Tag consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives this package's totals.
 * @return True when the header, the whole entry table and every visitor call work.
 */
[[nodiscard]] bool scan_package(std::wstring_view directory,
                                std::uint16_t packageId,
                                std::uint32_t classId,
                                ClassEntryVisitor visitor,
                                void* context,
                                ScanResult& result) noexcept {
    Path stem{};
    std::uint32_t patchIndex = 0;
    if (!find_latest(directory, packageId, stem, patchIndex)) {
        return false;
    }
    const std::wstring_view fullStem(stem.chars.data());
    const std::size_t separator = fullStem.find_last_of(L"\\/");
    const std::wstring_view packageFamily =
        separator == std::wstring_view::npos ? fullStem : fullStem.substr(separator + 1U);
    if (packageFamily.empty()) {
        return false;
    }
    Path path{};
    std::array<std::byte, layout::kHeaderSize> headerBytes{};
    Header header{};
    if (!build_path(stem, patchIndex, path) || !read_at(path, 0, headerBytes)
        || !parse_header(headerBytes, header)) {
        return false;
    }
    // A larger table would produce entry indices that no tag handle can address.
    if (header.entryCount > kMaximumEntries) {
        return false;
    }
    ++result.packages;

    std::array<layout::EntryRecord, kBatchRecords> batch{};
    for (std::uint32_t index = 0; index < header.entryCount;) {
        const auto remaining = static_cast<std::size_t>(header.entryCount - index);
        const std::size_t count = (std::min)(batch.size(), remaining);
        const std::uint64_t offset =
            header.entryTable + static_cast<std::uint64_t>(index) * sizeof(layout::EntryRecord);
        if (!read_at(path, offset, std::as_writable_bytes(std::span{batch.data(), count}))) {
            return false;
        }
        for (std::size_t position = 0; position < count; ++position) {
            ++result.entries;
            if (batch[position].reference != classId) {
                continue;
            }
            ++result.matches;
            const std::uint32_t entryIndex = index + static_cast<std::uint32_t>(position);
            const std::uint32_t tag =
                layout::kTagBase + (static_cast<std::uint32_t>(packageId) << layout::kTagEntryBits)
                + entryIndex;
            if (!visitor(context, ClassEntry{tag, packageFamily})) {
                return false;
            }
        }
        index += static_cast<std::uint32_t>(count);
    }
    return true;
}

/** Legacy tag-only visitor and its caller context. */
struct LegacyVisitor {
    ClassVisitor visitor{};
    void* context{};
};

/** @param context Legacy visitor. @param entry Detailed match. @return Visitor outcome. */
[[nodiscard]] bool visit_legacy(void* context, const ClassEntry& entry) noexcept {
    const auto& legacy = *static_cast<const LegacyVisitor*>(context);
    return legacy.visitor(legacy.context, entry.tag);
}

} // namespace

/** Reports every installed entry of one tag class. */
bool scan_class(std::wstring_view directory,
                std::uint32_t classId,
                ClassVisitor visitor,
                void* context,
                ScanResult& result) noexcept {
    result = {};
    if (visitor == nullptr) {
        return false;
    }
    LegacyVisitor legacy{visitor, context};
    return scan_class_entries(directory, classId, &visit_legacy, &legacy, result);
}

/** Reports every installed entry of one tag class with its package family. */
bool scan_class_entries(std::wstring_view directory,
                        std::uint32_t classId,
                        ClassEntryVisitor visitor,
                        void* context,
                        ScanResult& result) noexcept {
    result = {};
    if (directory.empty() || visitor == nullptr) {
        return false;
    }
    Path search{};
    if (!search_pattern(directory, search)) {
        return false;
    }
    WIN32_FIND_DATAW found{};
    const HANDLE enumeration = FindFirstFileW(search.chars.data(), &found);
    if (enumeration == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Patches of one package repeat its id, and only the highest is scanned.
    SeenSet seen{};
    bool complete = true;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::uint16_t packageId = 0;
        std::uint32_t patchIndex = 0;
        if (!parse_leaf(found.cFileName, packageId, patchIndex) || !claim(seen, packageId)) {
            continue;
        }
        if (!scan_package(directory, packageId, classId, visitor, context, result)) {
            complete = false;
            break;
        }
    } while (FindNextFileW(enumeration, &found) != 0);
    if (complete && GetLastError() != ERROR_NO_MORE_FILES) {
        complete = false;
    }
    return FindClose(enumeration) != FALSE && complete;
}

} // namespace sunrise::middleware::content::packages::reader
