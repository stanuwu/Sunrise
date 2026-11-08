#include <algorithm>
#include <cwchar>
#include <string_view>

#include "handle_cache.h"
#include "internal.h"
#include "locator_cache.h"
#include "overlay.h"
#include "package_file.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

/**
 * Copies a directory and leaf into one fixed path buffer.
 * @param directory Installed packages directory, with or without a trailing separator.
 * @param leaf Leaf text appended after one separator.
 * @param path Receives the full path.
 * @return True when the result and its null fit.
 */
[[nodiscard]] bool join(std::wstring_view directory, std::wstring_view leaf, Path& path) noexcept {
    const bool separated = !directory.empty() && directory.back() == L'\\';
    const std::size_t total = directory.size() + (separated ? 0U : 1U) + leaf.size();
    if (total + 1U > path.chars.size()) {
        return false;
    }
    std::size_t cursor = directory.size();
    std::copy_n(directory.data(), directory.size(), path.chars.data());
    if (!separated) {
        path.chars[cursor++] = L'\\';
    }
    std::copy_n(leaf.data(), leaf.size(), path.chars.data() + cursor);
    path.chars[cursor + leaf.size()] = L'\0';
    return true;
}

/** @param value One character. @param digit Receives its value. @return True for a hex digit. */
[[nodiscard]] bool hex_digit(wchar_t value, std::uint16_t& digit) noexcept {
    if (value >= L'0' && value <= L'9') {
        digit = static_cast<std::uint16_t>(value - L'0');
    } else if (value >= L'a' && value <= L'f') {
        digit = static_cast<std::uint16_t>(value - L'a' + 10);
    } else if (value >= L'A' && value <= L'F') {
        digit = static_cast<std::uint16_t>(value - L'A' + 10);
    } else {
        return false;
    }
    return true;
}

/**
 * Copies the stem of one known file name, dropping its patch suffix and extension.
 * @param fileName Whole leaf name.
 * @param stem Receives the stem text.
 * @return True when the name has a patch suffix and the stem fits.
 */
[[nodiscard]] bool copy_stem(std::wstring_view fileName, Path& stem) noexcept {
    const std::size_t extension = fileName.rfind(L'.');
    if (extension == std::wstring_view::npos) {
        return false;
    }
    const std::size_t suffix = fileName.rfind(L'_', extension);
    if (suffix == std::wstring_view::npos || suffix + 1U > stem.chars.size()) {
        return false;
    }
    std::copy_n(fileName.data(), suffix, stem.chars.data());
    stem.chars[suffix] = L'\0';
    return true;
}

} // namespace

/** Parses the package id and patch index out of one leaf name. */
bool parse_leaf(std::wstring_view fileName,
                std::uint16_t& packageId,
                std::uint32_t& patchIndex) noexcept {
    packageId = 0;
    patchIndex = 0;
    /** The package id is written as exactly 4 hex digits. */
    constexpr std::size_t kIdentifierDigits = 4;
    if (fileName.size() <= layout::kPackageFileExtension.size()
        || !fileName.ends_with(layout::kPackageFileExtension)) {
        return false;
    }
    std::wstring_view stem =
        fileName.substr(0, fileName.size() - layout::kPackageFileExtension.size());
    const std::size_t patchSeparator = stem.rfind(L'_');
    if (patchSeparator == std::wstring_view::npos || patchSeparator + 1 >= stem.size()) {
        return false;
    }
    for (const wchar_t value : stem.substr(patchSeparator + 1)) {
        if (value < L'0' || value > L'9') {
            return false;
        }
        patchIndex = patchIndex * 10U + static_cast<std::uint32_t>(value - L'0');
    }
    stem = stem.substr(0, patchSeparator);
    const std::size_t identifierSeparator = stem.rfind(L'_');
    if (identifierSeparator == std::wstring_view::npos
        || stem.size() - identifierSeparator - 1 != kIdentifierDigits) {
        return false;
    }
    for (const wchar_t value : stem.substr(identifierSeparator + 1)) {
        std::uint16_t digit = 0;
        if (!hex_digit(value, digit)) {
            return false;
        }
        packageId = static_cast<std::uint16_t>(packageId * 16U + digit);
    }
    return true;
}

namespace {

/**
 * Reports the table row that holds or may hold one package id.
 * The table is sized on first use and never erases a row, so a forward probe finds every held id.
 * @param scratch Reader whose location table is used.
 * @param packageId Package id being resolved.
 * @return The row, or null when the table cannot be sized or holds no free row.
 */
[[nodiscard]] PackageLocationSlot* location_slot(Scratch& scratch,
                                                 std::uint16_t packageId) noexcept {
    if (scratch.packageLocations.empty()) {
        try {
            scratch.packageLocations.resize(kPackageLocationSlots);
        } catch (...) {
            scratch.packageLocations.clear();
            return nullptr;
        }
    }
    const std::size_t rows = scratch.packageLocations.size();
    std::size_t probe = static_cast<std::size_t>(packageId) % rows;
    for (std::size_t step = 0; step < rows; ++step) {
        PackageLocationSlot& slot = scratch.packageLocations[probe];
        if (!slot.held || slot.packageId == packageId) {
            return &slot;
        }
        probe = probe + 1U == rows ? 0U : probe + 1U;
    }
    return nullptr;
}

} // namespace

/** @param scratch Reader whose held package locations are dropped. */
void release_locations(Scratch& scratch) noexcept {
    for (PackageLocationSlot& slot : scratch.packageLocations) {
        slot.held = false;
    }
}

namespace {

/** Finds the highest installed patch of one package id. */
[[nodiscard]] bool find_installed(std::wstring_view directory,
                                  std::uint16_t packageId,
                                  Path& stem,
                                  std::uint32_t& patchIndex) noexcept {
    stem = {};
    patchIndex = 0;
    const std::uint64_t directoryKey = locator_cache::directory_hash(directory);
    if (locator_cache::find(directoryKey, packageId, stem, patchIndex)) {
        return true;
    }
    // A directory already read answers a miss on its own. The package is not installed.
    if (locator_cache::complete(directoryKey)) {
        return false;
    }
    Path filter{};
    if (!join(directory, L"*.pkg", filter)) {
        return false;
    }
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(filter.chars.data(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        std::uint16_t candidateId = 0;
        std::uint32_t candidatePatch = 0;
        Path leaf{};
        Path candidate{};
        if (!parse_leaf(found.cFileName, candidateId, candidatePatch)
            || !copy_stem(found.cFileName, leaf)
            || !join(directory, std::wstring_view(leaf.chars.data()), candidate)) {
            continue;
        }
        locator_cache::store(directoryKey, candidateId, candidate, candidatePatch);
    } while (FindNextFileW(search, &found) != FALSE);
    const bool enumerated = GetLastError() == ERROR_NO_MORE_FILES;
    const bool closed = FindClose(search) != FALSE;
    if (!enumerated || !closed) {
        return false;
    }
    locator_cache::mark_complete(directoryKey);
    return locator_cache::find(directoryKey, packageId, stem, patchIndex);
}

} // namespace

/** A mod-served generation above the installed ones becomes the latest. */
bool find_latest(std::wstring_view directory,
                 bool installedOnly,
                 std::uint16_t packageId,
                 Path& stem,
                 std::uint32_t& patchIndex) noexcept {
    const bool installed = find_installed(directory, packageId, stem, patchIndex);
    std::uint32_t served = 0;
    if (installedOnly || !overlay::latest(packageId, served)) {
        return installed;
    }
    if (installed) {
        patchIndex = (std::max)(patchIndex, served);
        return true;
    }
    // A new package exists only in its mod, so its stem is the served file's.
    Path path{};
    if (!overlay::find(packageId, served, path)
        || !copy_stem(std::wstring_view(path.chars.data()), stem)) {
        return false;
    }
    patchIndex = served;
    return true;
}

namespace {

/**
 * Drops every cache whose key omits the source directory or read mode.
 * Headers, tables and blocks are keyed by package and patch, and a mod can serve the same patch
 * index as an installed file. Open files are keyed by path, so they stay.
 * @param scratch Reader whose caches are dropped.
 */
void drop_source_caches(Scratch& scratch) noexcept {
    release_locations(scratch);
    scratch.packageLocationFallback = {};
    for (HeaderSlot& slot : scratch.headers) {
        slot.valid = false;
    }
    // Dropped slots stay chained otherwise, and the next hold would chain one slot twice.
    clear_slot_index(scratch.tableIndex);
    for (TableSlot& slot : scratch.tables) {
        slot.occupied = false;
        slot.entryCount = 0;
        slot.blockCount = 0;
        slot.used = 0;
    }
    scratch.tableCursor = 0;
    clear_slot_index(scratch.blockIndex);
    for (BlockSlot& slot : scratch.blocks) {
        slot.valid = false;
    }
    scratch.blockCursor = 0;
}

} // namespace

/** Resolves one package once per reader, source directory and read mode. */
bool resolve_latest(Scratch& scratch,
                    const Source& source,
                    std::uint16_t packageId,
                    const PackageLocation*& output) noexcept {
    output = nullptr;
    const std::wstring_view directory = source.directory;
    // A new overlay table can move a package to another generation or file.
    if (const std::uint64_t epoch = overlay::epoch(); epoch != scratch.overlayEpoch) {
        drop_source_caches(scratch);
        scratch.overlayEpoch = epoch;
    }
    if (source.installedOnly != scratch.packageInstalledOnly) {
        drop_source_caches(scratch);
        scratch.packageInstalledOnly = source.installedOnly;
    }
    // The key is compared by content. A borrowed pointer identity could alias a freed and
    // reallocated directory string and serve stale locations.
    const bool sameDirectory =
        directory.size() != 0 && directory.size() == scratch.packageDirectoryLength
        && std::equal(directory.begin(), directory.end(), scratch.packageDirectory.chars.begin());
    if (!sameDirectory) {
        drop_source_caches(scratch);
        scratch.packageDirectory = {};
        scratch.packageDirectoryLength = 0;
        if (directory.size() < scratch.packageDirectory.chars.size()) {
            std::copy(directory.begin(), directory.end(), scratch.packageDirectory.chars.begin());
            scratch.packageDirectoryLength = directory.size();
        }
    }
    PackageLocationSlot* const slot = location_slot(scratch, packageId);
    if (slot != nullptr && slot->held) {
        ++scratch.packageLocationHits;
        output = slot->location.found ? &slot->location : nullptr;
        return slot->location.found;
    }

    ++scratch.packageLocationMisses;
    PackageLocation pending{};
    pending.found =
        find_latest(directory, source.installedOnly, packageId, pending.stem, pending.patchIndex)
        && build_path(
            pending.stem, packageId, pending.patchIndex, source.installedOnly, pending.latestPath);
    // A full or unallocated table keeps the result in the fallback, so the caller still gets it.
    PackageLocation& kept = slot != nullptr ? slot->location : scratch.packageLocationFallback;
    kept = pending;
    if (slot != nullptr) {
        slot->packageId = packageId;
        slot->held = true;
    }
    output = kept.found ? &kept : nullptr;
    return kept.found;
}

/** A mod-served generation is read from its own file; every other one sits beside the stem. */
bool build_path(const Path& stem,
                std::uint16_t packageId,
                std::uint32_t patchIndex,
                bool installedOnly,
                Path& path) noexcept {
    if (!installedOnly && overlay::find(packageId, patchIndex, path)) {
        return true;
    }
    path = {};
    const int written = std::swprintf(
        path.chars.data(), path.chars.size(), L"%s_%u.pkg", stem.chars.data(), patchIndex);
    return written > 0;
}

/** Reads an exact byte range from one file. */
bool read_at(const Path& path, std::uint64_t offset, std::span<std::byte> output) noexcept {
    return handle_cache::read(path, offset, output);
}

/** Reads an exact byte range through the files one reader keeps open. */
bool read_at(Scratch& scratch,
             const Path& path,
             std::uint64_t offset,
             std::span<std::byte> output) noexcept {
    return handle_cache::read(scratch, path, offset, output);
}

} // namespace sunrise::middleware::content::packages::reader
