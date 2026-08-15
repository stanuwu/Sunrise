#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::middleware::content::packages::named_tags {

/** Extracted names use bounded UTF-8 storage and reject truncation. */
inline constexpr std::size_t kNameCapacity = 128;

/** One named definition entry extracted from a package directory. */
struct Entry {
    std::array<char, kNameCapacity> name{};
    std::size_t nameLength{};
    std::uint32_t tag{};
    std::uint32_t classId{};
    /**
     * Patch index of the package file this entry came from.
     * A name appears once per patch with a different tag, so the highest patch holds the current
     * tag. A single-file extraction reports zero.
     */
    std::uint32_t patchIndex{};
};

/** Parsed package totals. No package storage is kept. */
struct Result {
    std::size_t entries{};
};

/** Totals for one whole package-directory extraction. */
struct DirectoryResult {
    std::size_t files{};
    std::size_t entries{};
};

/** Visitor called once per valid named definition. */
using Visitor = bool (*)(void* context, const Entry& entry) noexcept;

/**
 * Extracts named definitions from one installed package file.
 * @param path Null-terminated package path.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives parsed entry totals.
 * @return True when the header, table, strings, visitor, and file handling all work.
 */
[[nodiscard]] bool
extract_file(const wchar_t* path, Visitor visitor, void* context, Result& result) noexcept;

/**
 * Extracts named definitions from every package in one installed directory.
 * @param directory Package directory with or without a trailing separator.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives file and entry totals.
 * @return True when the walk and every package extraction work.
 */
[[nodiscard]] bool extract_directory(std::wstring_view directory,
                                     Visitor visitor,
                                     void* context,
                                     DirectoryResult& result) noexcept;

} // namespace sunrise::middleware::content::packages::named_tags
