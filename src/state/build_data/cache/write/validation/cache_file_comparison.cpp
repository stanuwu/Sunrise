#include "cache_file_comparison.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>

namespace sunrise::state::build_data::cache::validation {
namespace {

/** Two 4 KiB buffers limit stack use while comparing whole cache files. */
constexpr std::size_t kBufferCapacity = 4U * 1024U;
/** These share flags let another process keep or replace either name while we read. */
constexpr DWORD kReadShare = FILE_SHARE_READ | FILE_SHARE_DELETE;

} // namespace

/** Compares two closed cache files without keeping their contents or allocating. */
bool files_equal(const wchar_t* firstPath, const wchar_t* secondPath) noexcept {
    const HANDLE first = CreateFileW(firstPath,
                                     GENERIC_READ,
                                     kReadShare,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                     nullptr);
    if (first == INVALID_HANDLE_VALUE) {
        return false;
    }
    const HANDLE second = CreateFileW(secondPath,
                                      GENERIC_READ,
                                      kReadShare,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                      nullptr);
    if (second == INVALID_HANDLE_VALUE) {
        (void)CloseHandle(first);
        return false;
    }

    LARGE_INTEGER firstSize{};
    LARGE_INTEGER secondSize{};
    bool equal = GetFileSizeEx(first, &firstSize) != FALSE
                 && GetFileSizeEx(second, &secondSize) != FALSE && firstSize.QuadPart >= 0
                 && firstSize.QuadPart == secondSize.QuadPart;
    LONGLONG remaining = equal ? firstSize.QuadPart : 0;
    std::array<std::byte, kBufferCapacity> firstBuffer{};
    std::array<std::byte, kBufferCapacity> secondBuffer{};
    while (equal && remaining != 0) {
        const DWORD requested = static_cast<DWORD>(
            remaining < static_cast<LONGLONG>(firstBuffer.size()) ? remaining : firstBuffer.size());
        DWORD firstRead = 0;
        DWORD secondRead = 0;
        equal = ReadFile(first, firstBuffer.data(), requested, &firstRead, nullptr) != FALSE
                && ReadFile(second, secondBuffer.data(), requested, &secondRead, nullptr) != FALSE
                && firstRead == requested && secondRead == requested
                && std::memcmp(firstBuffer.data(), secondBuffer.data(), requested) == 0;
        remaining -= requested;
    }
    const bool secondClosed = CloseHandle(second) != FALSE;
    const bool firstClosed = CloseHandle(first) != FALSE;
    return equal && secondClosed && firstClosed;
}

} // namespace sunrise::state::build_data::cache::validation
