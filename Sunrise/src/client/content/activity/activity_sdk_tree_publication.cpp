#include "activity_sdk_tree_publication.h"

#include <Windows.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace sunrise::client::content::activity::sdk_generation::tree_publication {
namespace {

// Both separators end a path component.
constexpr std::wstring_view kSeparators = L"\\/";
// Suffixes of the sibling names one publication owns.
constexpr std::wstring_view kBackupSuffix = L".activity-sdk-backup";
constexpr std::wstring_view kPendingSuffix = L".activity-sdk-publication.pending";
constexpr std::wstring_view kCommittedSuffix = L".activity-sdk-publication.committed";
// Marker magic; the bytes spell AST1.
constexpr std::uint32_t kMarkerMagic = 0x31545341U;

struct Marker final {
    std::uint32_t magic{kMarkerMagic};
    std::uint8_t hadOutput{};
    std::array<std::uint8_t, 3> reserved{};
};

/** Resolves one null-terminated lexical path into normalized absolute storage. */
[[nodiscard]] bool full_path(const wchar_t* input, std::wstring& output) noexcept {
    output.clear();
    if (input == nullptr || input[0] == L'\0') {
        return false;
    }
    const DWORD needed = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (needed == 0) {
        return false;
    }
    try {
        std::wstring pending(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(input, needed, pending.data(), nullptr);
        if (written == 0 || written >= needed) {
            return false;
        }
        pending.resize(written);
        while (pending.size() > 3U && (pending.back() == L'\\' || pending.back() == L'/')) {
            pending.pop_back();
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Requires one existing ordinary directory. */
[[nodiscard]] bool is_directory(const wchar_t* path) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Requires one existing ordinary file. */
[[nodiscard]] bool is_file(const wchar_t* path) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/** Requires every existing drive-path directory component to be ordinary. */
[[nodiscard]] bool ordinary_ancestry(const std::wstring& directory) noexcept {
    if (directory.size() < 3U || directory[1] != L':' || directory[2] != L'\\'
        || !is_directory(directory.substr(0, 3U).c_str())) {
        return false;
    }
    std::size_t cursor = 3U;
    while (cursor < directory.size()) {
        const std::size_t separator = directory.find(L'\\', cursor);
        const std::size_t end = separator == std::wstring::npos ? directory.size() : separator;
        if (!is_directory(directory.substr(0, end).c_str())) {
            return false;
        }
        if (separator == std::wstring::npos) {
            break;
        }
        cursor = separator + 1U;
    }
    return true;
}

[[nodiscard]] bool same_text(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size()
           && CompareStringOrdinal(left.data(),
                                   static_cast<int>(left.size()),
                                   right.data(),
                                   static_cast<int>(right.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

/** Splits one path at its last separator. @return False when there is no interior separator. */
[[nodiscard]] bool
split(std::wstring_view path, std::wstring_view& parent, std::wstring_view& leaf) noexcept {
    const std::size_t separator = path.find_last_of(kSeparators);
    if (path.empty() || separator == std::wstring_view::npos || separator == 0
        || separator + 1 >= path.size()) {
        return false;
    }
    parent = path.substr(0, separator);
    leaf = path.substr(separator + 1);
    return true;
}

[[nodiscard]] bool default_move(void*, const wchar_t* source, const wchar_t* target) noexcept {
    return MoveFileExW(source, target, MOVEFILE_WRITE_THROUGH) != FALSE;
}

/** Deletes one directory tree. Refuses to follow a reparse point. @return True when gone. */
[[nodiscard]] bool remove_tree(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                   ? RemoveDirectoryW(path.c_str()) != FALSE
                   : DeleteFileW(path.c_str()) != FALSE;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return DeleteFileW(path.c_str()) != FALSE;
    }

    WIN32_FIND_DATAW entry{};
    const std::wstring search = path + L"\\*";
    const HANDLE handle = FindFirstFileW(search.c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND && RemoveDirectoryW(path.c_str()) != FALSE;
    }
    bool complete = true;
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring child = path + L"\\" + std::wstring(name);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            complete = remove_tree(child) && complete;
        } else if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            complete = RemoveDirectoryW(child.c_str()) != FALSE && complete;
        } else {
            complete = DeleteFileW(child.c_str()) != FALSE && complete;
        }
    } while (FindNextFileW(handle, &entry) != FALSE);
    complete = FindClose(handle) != FALSE && complete;
    return RemoveDirectoryW(path.c_str()) != FALSE && complete;
}

/** Appends one fixed transaction suffix without exposing a partially written result. */
[[nodiscard]] bool
sibling_path(std::wstring_view output, std::wstring_view suffix, std::wstring& sibling) noexcept {
    try {
        sibling.assign(output);
        sibling.append(suffix);
        return true;
    } catch (...) {
        sibling.clear();
        return false;
    }
}

/** Writes and flushes one bounded transaction marker before moving either tree. */
[[nodiscard]] bool write_marker(const std::wstring& path, bool hadOutput) noexcept {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const Marker marker{kMarkerMagic, static_cast<std::uint8_t>(hadOutput ? 1U : 0U), {}};
    DWORD written = 0;
    const bool complete = WriteFile(file, &marker, sizeof marker, &written, nullptr) != FALSE
                          && written == sizeof marker && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!complete || !closed) {
        (void)DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

/** Reads one exact marker without accepting trailing bytes or noncanonical fields. */
[[nodiscard]] bool read_marker(const std::wstring& path, Marker& output) noexcept {
    output = {};
    if (!is_file(path.c_str())) {
        return false;
    }
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD read = 0;
    std::byte trailing{};
    DWORD trailingRead = 0;
    const bool complete =
        ReadFile(file, &output, sizeof output, &read, nullptr) != FALSE && read == sizeof output
        && ReadFile(file, &trailing, 1, &trailingRead, nullptr) != FALSE && trailingRead == 0;
    const bool closed = CloseHandle(file) != FALSE;
    return complete && closed && output.magic == kMarkerMagic && output.hadOutput <= 1U
           && output.reserved == std::array<std::uint8_t, 3>{};
}

/** Accepts one absent tree or requires an ordinary directory leaf. */
[[nodiscard]] bool tree_state(const std::wstring& path, bool& exists) noexcept {
    exists = false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    exists = true;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Restores or finalizes one interrupted transaction before any new publication begins. */
[[nodiscard]] bool recover(const std::wstring& output,
                           const std::wstring& backup,
                           const std::wstring& pending,
                           const std::wstring& committed) noexcept {
    const DWORD pendingAttributes = GetFileAttributesW(pending.c_str());
    const DWORD committedAttributes = GetFileAttributesW(committed.c_str());
    if (pendingAttributes != INVALID_FILE_ATTRIBUTES
        && committedAttributes != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    bool outputExists = false;
    bool backupExists = false;
    if (!tree_state(output, outputExists) || !tree_state(backup, backupExists)) {
        return false;
    }
    if (committedAttributes != INVALID_FILE_ATTRIBUTES) {
        Marker marker{};
        if (!read_marker(committed, marker) || !outputExists) {
            return false;
        }
        if (backupExists && !remove_tree(backup)) {
            return false;
        }
        return DeleteFileW(committed.c_str()) != FALSE;
    }
    if (pendingAttributes == INVALID_FILE_ATTRIBUTES) {
        return !backupExists;
    }
    Marker marker{};
    if (!read_marker(pending, marker)) {
        return false;
    }
    if (backupExists) {
        if ((outputExists && !remove_tree(output))
            || !default_move(nullptr, backup.c_str(), output.c_str())) {
            return false;
        }
    } else if (marker.hadOutput != 0 && !outputExists) {
        return false;
    }
    return DeleteFileW(pending.c_str()) != FALSE;
}

} // namespace

/** @return The stable log name of one publication status. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::ready:
        return "ready";
    case Status::invalidInput:
        return "invalid_input";
    case Status::backupCollision:
        return "backup_collision";
    case Status::backupFailure:
        return "backup_failure";
    case Status::commitFailure:
        return "commit_failure";
    case Status::rollbackFailure:
        return "rollback_failure";
    }
    return "invalid_input";
}

/** Moves the stage tree over the output tree, restoring the old tree if the commit fails. */
Status publish(const wchar_t* stage,
               const wchar_t* output,
               MoveOperation move,
               void* moveContext) noexcept {
    if (stage == nullptr || stage[0] == L'\0' || output == nullptr || output[0] == L'\0') {
        return Status::invalidInput;
    }
    std::wstring canonicalStage;
    std::wstring canonicalOutput;
    if (!full_path(stage, canonicalStage) || !full_path(output, canonicalOutput)) {
        return Status::invalidInput;
    }
    const std::wstring_view stageView(canonicalStage);
    const std::wstring_view outputView(canonicalOutput);
    std::wstring_view stageParent;
    std::wstring_view stageLeaf;
    std::wstring_view outputParent;
    std::wstring_view outputLeaf;
    if (!split(stageView, stageParent, stageLeaf) || !split(outputView, outputParent, outputLeaf)
        || !same_text(stageParent, outputParent) || same_text(stageLeaf, outputLeaf)
        || stageLeaf == L"." || stageLeaf == L".." || outputLeaf == L"." || outputLeaf == L"..") {
        return Status::invalidInput;
    }
    std::wstring parent;
    try {
        parent.assign(stageParent);
    } catch (...) {
        return Status::invalidInput;
    }
    if (!ordinary_ancestry(parent) || !is_directory(canonicalStage.c_str())) {
        return Status::invalidInput;
    }
    std::wstring backup;
    std::wstring pending;
    std::wstring committed;
    if (!sibling_path(outputView, kBackupSuffix, backup)
        || !sibling_path(outputView, kPendingSuffix, pending)
        || !sibling_path(outputView, kCommittedSuffix, committed)
        || !recover(canonicalOutput, backup, pending, committed)) {
        return Status::invalidInput;
    }
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return Status::backupCollision;
    }
    bool hadOutput = false;
    if (!tree_state(canonicalOutput, hadOutput) || !write_marker(pending, hadOutput)) {
        return Status::invalidInput;
    }
    const MoveOperation rename = move != nullptr ? move : &default_move;
    if (hadOutput && !rename(moveContext, canonicalOutput.c_str(), backup.c_str())) {
        (void)DeleteFileW(pending.c_str());
        return Status::backupFailure;
    }
    if (!rename(moveContext, canonicalStage.c_str(), canonicalOutput.c_str())) {
        if (hadOutput && !rename(moveContext, backup.c_str(), canonicalOutput.c_str())) {
            return Status::rollbackFailure;
        }
        (void)DeleteFileW(pending.c_str());
        return Status::commitFailure;
    }
    if (!rename(moveContext, pending.c_str(), committed.c_str())) {
        const bool removed = remove_tree(canonicalOutput);
        const bool restored =
            !hadOutput || rename(moveContext, backup.c_str(), canonicalOutput.c_str());
        if (removed && restored) {
            (void)DeleteFileW(pending.c_str());
            return Status::commitFailure;
        }
        return Status::rollbackFailure;
    }
    if ((!hadOutput || remove_tree(backup)) && DeleteFileW(committed.c_str()) != FALSE) {
        return Status::ready;
    }
    return Status::ready;
}

bool discard(const wchar_t* stage) noexcept {
    if (stage == nullptr || stage[0] == L'\0') {
        return false;
    }
    try {
        return remove_tree(std::wstring(stage));
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::tree_publication
