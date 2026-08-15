#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "reader.h"

namespace sunrise::middleware::content::packages::reader {

/** Header fields the package readers use. */
struct Header {
    std::uint16_t version{};
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    std::uint32_t entryCount{};
    std::uint32_t blockCount{};
    std::uint64_t entryTable{};
    std::uint64_t blockTable{};
};

/**
 * Parses the public header prefix.
 * @param bytes Whole header prefix.
 * @param header Receives the fields it reads.
 * @return True for a supported version whose table offsets fit.
 */
[[nodiscard]] bool parse_header(std::span<const std::byte, layout::kHeaderSize> bytes,
                                Header& header) noexcept;

/**
 * Parses the package id and patch index out of one leaf name.
 * @param fileName Whole leaf name.
 * @param packageId Receives the package id.
 * @param patchIndex Receives the patch index.
 * @return True when the leaf has that exact form.
 */
[[nodiscard]] bool parse_leaf(std::wstring_view fileName,
                              std::uint16_t& packageId,
                              std::uint32_t& patchIndex) noexcept;

/**
 * Finds the highest-patch file for one package id.
 * @param directory Installed packages directory.
 * @param packageId Package id from the tag handle.
 * @param stem Receives the file stem without its patch suffix and extension.
 * @param patchIndex Receives the highest patch index found.
 * @return True when at least one matching package exists.
 */
[[nodiscard]] bool find_latest(std::wstring_view directory,
                               std::uint16_t packageId,
                               Path& stem,
                               std::uint32_t& patchIndex) noexcept;

/**
 * Builds the full path of one patch of a package stem.
 * @param stem Stem produced by find_latest.
 * @param patchIndex Requested patch index.
 * @param path Receives the full path.
 * @return True when the path fits fixed storage.
 */
[[nodiscard]] bool build_path(const Path& stem, std::uint32_t patchIndex, Path& path) noexcept;

/**
 * Reads an exact byte range from one file, through files no reader owns.
 * The class sweeps use this. It locks, so nothing on a parallel read path may call it.
 * @param path Full package path.
 * @param offset File offset.
 * @param output Exact destination.
 * @return True when every requested byte is read.
 */
[[nodiscard]] bool
read_at(const Path& path, std::uint64_t offset, std::span<std::byte> output) noexcept;

/**
 * Reads an exact byte range through the files one reader keeps open.
 * Nothing here is shared, so several readers run this at once without locking.
 * @param scratch The reader's own storage.
 * @param path Full package path.
 * @param offset File offset.
 * @param output Exact destination.
 * @return True when every requested byte is read.
 */
[[nodiscard]] bool read_at(Scratch& scratch,
                           const Path& path,
                           std::uint64_t offset,
                           std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::content::packages::reader
