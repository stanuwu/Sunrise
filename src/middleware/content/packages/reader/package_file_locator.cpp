#include <algorithm>
#include <cwchar>
#include <string_view>

#include "handle_cache.h"
#include "internal.h"
#include "locator_cache.h"

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
    /** Every installed package file carries this extension. */
    constexpr std::wstring_view kExtension = L".pkg";
    /** The package id is written as exactly 4 hex digits. */
    constexpr std::size_t kIdentifierDigits = 4;
    if (fileName.size() <= kExtension.size() || !fileName.ends_with(kExtension)) {
        return false;
    }
    std::wstring_view stem = fileName.substr(0, fileName.size() - kExtension.size());
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

/** Finds the highest-patch file for one package id. */
bool find_latest(std::wstring_view directory,
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

/** Builds the full path of one patch of a package stem. */
bool build_path(const Path& stem, std::uint32_t patchIndex, Path& path) noexcept {
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
