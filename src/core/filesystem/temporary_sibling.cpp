#include "temporary_sibling.h"

#include <Windows.h>

#include <cstddef>
#include <string_view>

#include "path.h"

namespace sunrise::core::path {
namespace {

/** Writer-owned temporary names use this exact lowercase extension. */
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** Dots separate the final name and all three writer identity components. */
constexpr std::wstring_view kComponentSeparator = L".";
/** The search stays limited to temporary siblings of the one final name. */
constexpr std::wstring_view kSearchSuffix = L".*.tmp";
/** Both Windows path separator forms are accepted when rebuilding a sibling path. */
constexpr std::wstring_view kDirectorySeparators = L"\\/";
/** One path separator character precedes a Windows leaf name. */
constexpr std::size_t kPathSeparatorWidth = 1;
/** One byte is 2 hex path characters. */
constexpr std::size_t kHexadecimalDigitsPerByte = 2;
/** Each Windows DWORD id is 8 hex path characters. */
constexpr std::size_t kWriterIdentifierDigits = sizeof(DWORD) * kHexadecimalDigitsPerByte;
/** Process, thread, and sequence ids form every writer-owned name. */
constexpr std::size_t kWriterComponentCount = 3;
/** The first id component is the process that owns the temporary file. */
constexpr std::size_t kProcessComponentIndex = 0;
/** Each hex path character is 4 bits. */
constexpr std::size_t kBitsPerHexadecimalDigit = 4;
/** Letter hex digits start after the 10 decimal digits. */
constexpr DWORD kAlphabeticHexadecimalOffset = 10;
/** 64 sibling entries cap directory work during one write. */
constexpr std::size_t kMaximumEntriesPerSweep = 64;
/** 8 removals spread stale-file cleanup across later writes. */
constexpr std::size_t kMaximumRemovalsPerSweep = 8;
/** A zero-duration wait observes process state without blocking a write. */
constexpr DWORD kNonBlockingWaitMilliseconds = 0;
/** Windows FILETIME stores 10 million 100-nanosecond ticks per second. */
constexpr ULONGLONG kFileTimeTicksPerSecond = 10'000'000ULL;
/** One hour keeps recent ownership transitions outside crash-residue cleanup. */
constexpr ULONGLONG kMinimumStaleAgeSeconds = 60ULL * 60ULL;
/** The age threshold is converted once into the native FILETIME tick unit. */
constexpr ULONGLONG kMinimumStaleAgeTicks = kMinimumStaleAgeSeconds * kFileTimeTicksPerSecond;

/**
 * Converts one uppercase hex path character to its value.
 * @param value Receives 0 through 15.
 * @return True when the character is in the writer's uppercase alphabet.
 */
[[nodiscard]] bool hexadecimal_value(wchar_t character, DWORD& value) noexcept {
    if (character >= L'0' && character <= L'9') {
        value = static_cast<DWORD>(character - L'0');
        return true;
    }
    if (character >= L'A' && character <= L'F') {
        value = static_cast<DWORD>(character - L'A') + kAlphabeticHexadecimalOffset;
        return true;
    }
    return false;
}

/**
 * Parses one fixed-width Windows writer identifier.
 * @param text 8 uppercase hex path characters.
 * @param value Receives the parsed DWORD id.
 * @return True when every character is valid and the width is exact.
 */
[[nodiscard]] bool parse_identifier(std::wstring_view text, DWORD& value) noexcept {
    if (text.size() != kWriterIdentifierDigits) {
        return false;
    }
    value = 0;
    for (const wchar_t character : text) {
        DWORD digit = 0;
        if (!hexadecimal_value(character, digit)) {
            return false;
        }
        value = (value << kBitsPerHexadecimalDigit) | digit;
    }
    return true;
}

/**
 * Validates one exact writer-owned leaf name and extracts its process identity.
 * @param finalName Final leaf name without its directory.
 * @param candidate Enumerated sibling leaf name.
 * @param processId Receives the owning process id.
 * @return True only for the final.process.thread.sequence.tmp writer layout.
 */
[[nodiscard]] bool
parse_owner(std::wstring_view finalName, std::wstring_view candidate, DWORD& processId) noexcept {
    const std::size_t writerSuffixLength =
        kWriterComponentCount * (kComponentSeparator.size() + kWriterIdentifierDigits)
        + kTemporarySuffix.size();
    if (candidate.size() != finalName.size() + writerSuffixLength
        || candidate.substr(0, finalName.size()) != finalName) {
        return false;
    }

    std::size_t offset = finalName.size();
    for (std::size_t component = 0; component < kWriterComponentCount; ++component) {
        if (candidate.substr(offset, kComponentSeparator.size()) != kComponentSeparator) {
            return false;
        }
        offset += kComponentSeparator.size();
        DWORD identifier = 0;
        if (!parse_identifier(candidate.substr(offset, kWriterIdentifierDigits), identifier)) {
            return false;
        }
        if (component == kProcessComponentIndex) {
            processId = identifier;
        }
        offset += kWriterIdentifierDigits;
    }
    return candidate.substr(offset) == kTemporarySuffix;
}

/** @param value Native FILETIME value. @return Unsigned 100-nanosecond tick count. */
[[nodiscard]] ULONGLONG file_time_ticks(const FILETIME& value) noexcept {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

/**
 * Checks the age threshold without treating future timestamps as stale.
 * @param lastWrite Candidate's last-write time from directory enumeration.
 * @param now Current system time in the same FILETIME clock.
 * @return True when at least one cleanup grace period has elapsed.
 */
[[nodiscard]] bool old_enough(const FILETIME& lastWrite, const FILETIME& now) noexcept {
    const ULONGLONG writtenTicks = file_time_ticks(lastWrite);
    const ULONGLONG currentTicks = file_time_ticks(now);
    return currentTicks >= writtenTicks && currentTicks - writtenTicks >= kMinimumStaleAgeTicks;
}

/**
 * Confirms an owner has stopped without guessing through access failures.
 * @param processId Process id parsed from the temporary name.
 * @return True only when Windows reports no process or a signaled process object.
 */
[[nodiscard]] bool owner_stopped(DWORD processId) noexcept {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD waitResult = WaitForSingleObject(process, kNonBlockingWaitMilliseconds);
    (void)CloseHandle(process);
    return waitResult == WAIT_OBJECT_0;
}

/**
 * Rebuilds an enumerated sibling path below the final file's directory.
 * @param finalPath Complete final file path.
 * @param leafOffset Offset of the final leaf within that path.
 * @param candidateName Enumerated sibling leaf name.
 * @param candidatePath Receives the complete sibling path.
 * @return True when the complete path and null fit fixed storage.
 */
[[nodiscard]] bool make_candidate_path(std::wstring_view finalPath,
                                       std::size_t leafOffset,
                                       std::wstring_view candidateName,
                                       Buffer& candidatePath) noexcept {
    return assign(candidatePath, finalPath.substr(0, leafOffset))
           && append(candidatePath, candidateName);
}

} // namespace

/** Removes bounded, old writer-owned temporary siblings whose process has stopped. */
void remove_stale_siblings(const wchar_t* finalPath) noexcept {
    if (finalPath == nullptr) {
        return;
    }
    const std::wstring_view completePath(finalPath);
    if (completePath.empty()) {
        return;
    }
    const std::size_t separator = completePath.find_last_of(kDirectorySeparators);
    const std::size_t leafOffset =
        separator == std::wstring_view::npos ? 0 : separator + kPathSeparatorWidth;
    const std::wstring_view finalName = completePath.substr(leafOffset);

    Buffer searchPath;
    if (finalName.empty() || !assign(searchPath, completePath)
        || !append(searchPath, kSearchSuffix)) {
        return;
    }

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW(searchPath.chars.data(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }

    std::size_t inspected = 0;
    std::size_t removed = 0;
    do {
        if (inspected >= kMaximumEntriesPerSweep || removed >= kMaximumRemovalsPerSweep) {
            break;
        }
        ++inspected;
        const std::wstring_view candidateName(entry.cFileName);
        DWORD ownerProcessId = 0;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            || !parse_owner(finalName, candidateName, ownerProcessId)
            || !old_enough(entry.ftLastWriteTime, now) || !owner_stopped(ownerProcessId)) {
            continue;
        }

        Buffer candidatePath;
        if (!make_candidate_path(completePath, leafOffset, candidateName, candidatePath)) {
            continue;
        }
        // Writer handles deny delete sharing, so an active write remains protected here.
        if (DeleteFileW(candidatePath.chars.data()) != FALSE) {
            ++removed;
        }
    } while (FindNextFileW(search, &entry) != FALSE);
    (void)FindClose(search);
}

} // namespace sunrise::core::path
