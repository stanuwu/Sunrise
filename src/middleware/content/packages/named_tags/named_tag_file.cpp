#include <Windows.h>

#include <limits>

#include "../../../../core/filesystem/path.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::named_tags {
namespace {

/** The Windows file walk accepts only the installed .pkg extension. */
constexpr std::wstring_view kPackageWildcard = L"*.pkg";
/** Package filenames are joined under their caller-chosen directory. */
constexpr std::wstring_view kPathSeparator = L"\\";

/**
 * Reads one exact file range through an owned Windows package handle.
 * @param context Pointer to the open HANDLE value.
 * @param offset Absolute package byte offset.
 * @param output Exact caller destination.
 * @return True when Windows reads every requested byte.
 */
[[nodiscard]] bool
read_file(void* context, std::uint64_t offset, std::span<std::byte> output) noexcept {
    if (context == nullptr || output.empty()
        || offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())
        || output.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    const HANDLE file = *static_cast<const HANDLE*>(context);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE
           && ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &read, nullptr)
                  != FALSE
           && read == static_cast<DWORD>(output.size());
}

} // namespace

/**
 * Opens, parses, and closes one package through Windows file APIs.
 * @param path Null-terminated package path.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives parsed entry totals.
 * @return True when parsing works and the file handle closes cleanly.
 */
bool extract_file(const wchar_t* path, Visitor visitor, void* context, Result& result) noexcept {
    result = {};
    if (path == nullptr) {
        return false;
    }
    HANDLE file = CreateFileW(path,
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0;
    const Reader reader{
        &file,
        sized ? static_cast<std::uint64_t>(size.QuadPart) : 0,
        &read_file,
    };
    const bool extracted = sized && extract(reader, visitor, context, result);
    const bool closed = CloseHandle(file) != FALSE;
    return extracted && closed;
}

/**
 * Walks each package and streams its extracted entries to one visitor.
 * @param directory Package directory with or without a trailing separator.
 * @param visitor Required non-owning entry consumer.
 * @param context Caller context passed to the visitor.
 * @param result Receives file and entry totals.
 * @return True when every file handle and the walk handle close cleanly.
 */
bool extract_directory(std::wstring_view directory,
                       Visitor visitor,
                       void* context,
                       DirectoryResult& result) noexcept {
    PatchStamp stamp{};
    result = {};
    if (directory.empty() || visitor == nullptr) {
        return false;
    }
    core::path::Buffer base;
    if (!core::path::assign(base, directory)
        || (base.chars[base.length - 1] != L'\\' && !core::path::append(base, kPathSeparator))) {
        return false;
    }
    core::path::Buffer search = base;
    if (!core::path::append(search, kPackageWildcard)) {
        return false;
    }

    WIN32_FIND_DATAW found{};
    const HANDLE enumeration = FindFirstFileW(search.chars.data(), &found);
    if (enumeration == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool complete = true;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        core::path::Buffer file = base;
        Result fileResult{};
        stamp.inner = visitor;
        stamp.context = context;
        stamp.patchIndex = leaf_patch_index(found.cFileName);
        if (!core::path::append(file, found.cFileName)
            || !extract_file(file.chars.data(), &stamp_patch, &stamp, fileResult)) {
            complete = false;
            break;
        }
        ++result.files;
        if (fileResult.entries > (std::numeric_limits<std::size_t>::max)() - result.entries) {
            complete = false;
            break;
        }
        result.entries += fileResult.entries;
    } while (FindNextFileW(enumeration, &found) != FALSE);

    const DWORD enumerationError = GetLastError();
    const bool closed = FindClose(enumeration) != FALSE;
    return complete && enumerationError == ERROR_NO_MORE_FILES && closed;
}

} // namespace sunrise::middleware::content::packages::named_tags
