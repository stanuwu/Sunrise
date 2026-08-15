#include "content_manifest_package_header.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../../core/filesystem/path.h"

namespace sunrise::state::content_manifest::scanner::package {
namespace {

/** Installed packages for this supported build carry header version 38. */
constexpr std::uint16_t kSupportedHeaderVersion = 38;
/** Installed Windows packages name platform 2 in the public header prefix. */
constexpr std::uint16_t kSupportedPlatform = 2;
/** The public package header prefix ends after its 8-byte build signature. */
constexpr std::size_t kPublicHeaderPrefixSize = 16;
/** Package stems become file paths by adding the native `.pkg` extension. */
constexpr std::wstring_view kPackageExtension = L".pkg";

#pragma pack(push, 1)
/** Public prefix read from every installed package header. */
struct HeaderPrefix final {
    std::uint16_t version{};
    /** Platform 2 marks the Windows package layout, but is not a manifest field. */
    std::uint16_t platform{};
    std::uint16_t publicPackageId{};
    /** The 2 bytes at +0x06 belong to later package validation flags. */
    std::uint16_t validationFlags{};
    std::uint64_t buildSignature{};
};
#pragma pack(pop)

static_assert(sizeof(HeaderPrefix) == kPublicHeaderPrefixSize);

/** @param value Windows file time. @return Canonical unsigned 64-bit tick value. */
[[nodiscard]] std::uint64_t file_time_value(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
}

/**
 * Builds the exact package path represented by one canonical candidate.
 * @param directory Installed packages directory.
 * @param parsed Canonical parsed package name.
 * @param output Cleared fixed Windows path.
 * @return True when the directory, separator, stem, extension and null all fit.
 */
[[nodiscard]] bool make_path(std::wstring_view directory,
                             const ParsedName& parsed,
                             core::path::Buffer& output) noexcept {
    if (!core::path::assign(output, directory)) {
        return false;
    }
    if (output.length != 0 && output.chars[output.length - 1] != L'\\'
        && output.chars[output.length - 1] != L'/') {
        if (!core::path::append(output, L"\\")) {
            return false;
        }
    }
    if (output.length + parsed.stemLength + kPackageExtension.size() >= output.chars.size()) {
        return false;
    }
    for (std::size_t index = 0; index < parsed.stemLength; ++index) {
        output.chars[output.length++] = static_cast<wchar_t>(parsed.stem[index]);
    }
    output.chars[output.length] = L'\0';
    return core::path::append(output, kPackageExtension);
}

/**
 * Reads one fixed header prefix, and refuses a short but successful read.
 * @param file Open package file at offset zero.
 * @param output Cleared prefix destination.
 * @return True when Windows returns every required byte.
 */
[[nodiscard]] bool read_prefix(HANDLE file, HeaderPrefix& output) noexcept {
    output = {};
    DWORD copied = 0;
    return ReadFile(file, &output, sizeof output, &copied, nullptr) != FALSE
           && copied == sizeof output;
}

} // namespace

/** Reads one unchanged package header and copies its public build signature. */
bool read_header(std::wstring_view directory, Candidate& candidate) noexcept {
    candidate.buildSignature = 0;
    core::path::Buffer path;
    if (!make_path(directory, candidate.parsed, path)) {
        return false;
    }
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    FILETIME writeTime{};
    HeaderPrefix header{};
    const bool complete = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
                          && static_cast<std::uint64_t>(size.QuadPart) == candidate.fileSize
                          && GetFileTime(file, nullptr, nullptr, &writeTime) != FALSE
                          && file_time_value(writeTime) == candidate.lastWriteTime
                          && read_prefix(file, header) && header.version == kSupportedHeaderVersion
                          && header.platform == kSupportedPlatform
                          && header.publicPackageId == candidate.parsed.packageId;
    CloseHandle(file);
    if (!complete) {
        return false;
    }
    candidate.buildSignature = header.buildSignature;
    return true;
}

} // namespace sunrise::state::content_manifest::scanner::package
