#include "activity_sdk_live_publication.h"

#include <Windows.h>

#include <array>
#include <cwchar>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "activity_sdk_tree_publication.h"

namespace sunrise::client::content::activity::sdk_generation::live_publication {
namespace {

/** Names of the staging, backup and marker paths; recovery scans for exactly these. */
constexpr std::wstring_view kStageSuffix = L"\\.activity-sdk-stage.%08lX.%08lX.%08lX";
constexpr std::wstring_view kBackupSuffix = L".activity-sdk-backup";
constexpr std::wstring_view kPendingMarkerSuffix = L"\\.activity-sdk-publication.pending";
constexpr std::wstring_view kCommittedMarkerSuffix = L"\\.activity-sdk-publication.committed";
/** Distinct stage directories tried before giving up on a name collision. */
constexpr std::size_t kAllocationAttempts = 16;
/** Marker file magic, bytes "ASP1"; a file without it is not ours. */
constexpr std::uint32_t kMarkerMagic = 0x31505341U;

volatile LONG g_sequence{};

struct Resource final {
    const wchar_t* stage{};
    const wchar_t* final{};
    std::wstring backup{};
    bool directory{};
    bool hadFinal{};
    bool oldMoved{};
    bool newMoved{};
};

struct Marker final {
    std::uint32_t magic{kMarkerMagic};
    std::uint8_t priorMask{};
    std::array<std::uint8_t, 3> reserved{};
};

[[nodiscard]] bool ordinary_directory(const wchar_t* path) noexcept;

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

/** Requires every existing drive-path directory component to be ordinary, never a reparse point. */
[[nodiscard]] bool ordinary_ancestry(const std::wstring& directory) noexcept {
    if (directory.size() < 3U || directory[1] != L':' || directory[2] != L'\\') {
        return false;
    }
    if (!ordinary_directory(directory.substr(0, 3U).c_str())) {
        return false;
    }
    std::size_t cursor = 3U;
    while (cursor < directory.size()) {
        const std::size_t separator = directory.find(L'\\', cursor);
        const std::size_t end = separator == std::wstring::npos ? directory.size() : separator;
        const std::wstring prefix = directory.substr(0, end);
        if (!ordinary_directory(prefix.c_str())) {
            return false;
        }
        if (separator == std::wstring::npos) {
            break;
        }
        cursor = separator + 1U;
    }
    return true;
}

/** Resolves one existing ordinary directory and rejects reparse points in every ancestor. */
[[nodiscard]] bool canonical_directory(const wchar_t* input, std::wstring& output) noexcept {
    return full_path(input, output) && ordinary_ancestry(output);
}

/** Compares two complete Windows path components without locale-sensitive folding. */
[[nodiscard]] bool same_text(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size()
           && CompareStringOrdinal(left.data(),
                                   static_cast<int>(left.size()),
                                   right.data(),
                                   static_cast<int>(right.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

/** Checks an ordinal case-insensitive prefix without reading beyond either span. */
[[nodiscard]] bool starts_text(std::wstring_view value, std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size()
           && CompareStringOrdinal(value.data(),
                                   static_cast<int>(prefix.size()),
                                   prefix.data(),
                                   static_cast<int>(prefix.size()),
                                   TRUE)
                  == CSTR_EQUAL;
}

/** Splits one non-root path into borrowed parent and leaf spans. */
[[nodiscard]] bool
split(std::wstring_view path, std::wstring_view& parent, std::wstring_view& leaf) noexcept {
    const std::size_t separator = path.find_last_of(L"\\/");
    if (path.empty() || separator == std::wstring_view::npos || separator == 0
        || separator + 1U >= path.size()) {
        return false;
    }
    parent = path.substr(0, separator);
    leaf = path.substr(separator + 1U);
    return true;
}

/** Production rename primitive; every transaction path is same-volume by construction. */
[[nodiscard]] bool default_move(void*, const wchar_t* source, const wchar_t* target) noexcept {
    return MoveFileExW(source, target, MOVEFILE_WRITE_THROUGH) != FALSE;
}

/** Requires one ordinary directory and rejects a junction or symbolic-link leaf. */
[[nodiscard]] bool ordinary_directory(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/** Requires one ordinary file and rejects a reparse-backed leaf. */
[[nodiscard]] bool ordinary_file(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

/** Accepts a missing final path or checks its exact expected ordinary kind. */
[[nodiscard]] bool final_state(const wchar_t* path, bool directory, bool& exists) noexcept {
    exists = false;
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    exists = true;
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
           && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

/** Creates one empty ordinary directory or accepts an existing ordinary directory. */
[[nodiscard]] bool ensure_directory(const std::wstring& path) noexcept {
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS && ordinary_directory(path.c_str());
}

/** Builds one collision-resistant sibling backup path without consulting global artifact state. */
[[nodiscard]] bool backup_path(std::wstring_view finalPath, std::wstring& output) noexcept {
    try {
        output.assign(finalPath);
        output.append(kBackupSuffix);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Writes and flushes the bounded transaction marker before any final path moves. */
[[nodiscard]] bool write_marker(const std::wstring& path, std::uint8_t priorMask) noexcept {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const Marker marker{kMarkerMagic, priorMask, {}};
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

/** Reads one exact marker without accepting extra bytes or alternate state. */
[[nodiscard]] bool read_marker(const std::wstring& path, Marker& output) noexcept {
    output = {};
    if (!ordinary_file(path.c_str())) {
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
    return complete && closed && output.magic == kMarkerMagic
           && output.reserved == std::array<std::uint8_t, 3>{} && (output.priorMask & 0xF8U) == 0;
}

/** Removes one transaction-created final value before restoring its backup. */
[[nodiscard]] bool remove_value(const Resource& resource) noexcept {
    if (!resource.newMoved) {
        return true;
    }
    return resource.directory ? tree_publication::discard(resource.final)
                              : DeleteFileW(resource.final) != FALSE;
}

/** Restores every already-touched resource in reverse commit order. */
[[nodiscard]] bool rollback(std::span<Resource> resources,
                            std::size_t touched,
                            MoveOperation move,
                            void* moveContext) noexcept {
    bool complete = true;
    while (touched != 0) {
        Resource& resource = resources[--touched];
        if (resource.newMoved) {
            complete = remove_value(resource) && complete;
            resource.newMoved = false;
        }
        if (resource.oldMoved) {
            const bool restored = move(moveContext, resource.backup.c_str(), resource.final);
            complete = restored && complete;
            resource.oldMoved = !restored;
        }
    }
    return complete;
}

/** Moves one prior value aside and one staged value into its final name. */
[[nodiscard]] Status advance(std::span<Resource> resources,
                             std::size_t index,
                             MoveOperation move,
                             void* moveContext) noexcept {
    Resource& resource = resources[index];
    if (resource.hadFinal) {
        if (!move(moveContext, resource.final, resource.backup.c_str())) {
            return rollback(resources, index, move, moveContext) ? Status::backupFailure
                                                                 : Status::rollbackFailure;
        }
        resource.oldMoved = true;
    }
    if (!move(moveContext, resource.stage, resource.final)) {
        return rollback(resources, index + 1U, move, moveContext) ? Status::commitFailure
                                                                  : Status::rollbackFailure;
    }
    resource.newMoved = true;
    return Status::ready;
}

/** Deletes successful backup values; failure leaves only recoverable stale siblings. */
[[nodiscard]] bool discard_backups(std::span<const Resource> resources) noexcept {
    bool complete = true;
    for (const Resource& resource : resources) {
        if (!resource.hadFinal) {
            continue;
        }
        if (resource.directory) {
            complete = tree_publication::discard(resource.backup.c_str()) && complete;
        } else {
            complete = DeleteFileW(resource.backup.c_str()) != FALSE && complete;
        }
    }
    return complete;
}

/** Deletes one final path only when recovery proves it did not exist before the transaction. */
[[nodiscard]] bool remove_final(Resource& resource) noexcept {
    bool exists = false;
    if (!final_state(resource.final, resource.directory, exists)) {
        return false;
    }
    if (!exists) {
        return true;
    }
    resource.newMoved = true;
    const bool removed = remove_value(resource);
    resource.newMoved = false;
    return removed;
}

/** Resolves a prior interrupted transaction before a new stage can be allocated. */
[[nodiscard]] bool recover(const wchar_t* finalSdkDirectory,
                           const wchar_t* finalPackPath,
                           const wchar_t* finalCatalogPath) noexcept {
    try {
        std::wstring finalLua(finalSdkDirectory);
        finalLua.append(L"\\lua");
        std::array<Resource, 3> resources{
            Resource{nullptr, finalLua.c_str(), {}, true},
            Resource{nullptr, finalPackPath, {}, false},
            Resource{nullptr, finalCatalogPath, {}, false},
        };
        for (Resource& resource : resources) {
            if (!backup_path(resource.final, resource.backup)) {
                return false;
            }
        }
        const std::wstring pending = std::wstring(finalSdkDirectory) + kPendingMarkerSuffix.data();
        const std::wstring committed =
            std::wstring(finalSdkDirectory) + kCommittedMarkerSuffix.data();
        const DWORD pendingAttributes = GetFileAttributesW(pending.c_str());
        const DWORD committedAttributes = GetFileAttributesW(committed.c_str());
        if (pendingAttributes != INVALID_FILE_ATTRIBUTES
            && committedAttributes != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        if (committedAttributes != INVALID_FILE_ATTRIBUTES) {
            Marker committedMarker{};
            if (!read_marker(committed, committedMarker)) {
                return false;
            }
            for (Resource& resource : resources) {
                bool exists = false;
                if (!final_state(resource.final, resource.directory, exists) || !exists) {
                    return false;
                }
            }
            for (const Resource& resource : resources) {
                const DWORD attributes = GetFileAttributesW(resource.backup.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    continue;
                }
                const bool removed = resource.directory
                                         ? tree_publication::discard(resource.backup.c_str())
                                         : DeleteFileW(resource.backup.c_str()) != FALSE;
                if (!removed) {
                    return false;
                }
            }
            return DeleteFileW(committed.c_str()) != FALSE;
        }

        Marker marker{};
        std::uint8_t priorMask = 0;
        bool hasAnyBackup = false;
        for (const Resource& resource : resources) {
            hasAnyBackup |= GetFileAttributesW(resource.backup.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        if (pendingAttributes == INVALID_FILE_ATTRIBUTES && !hasAnyBackup) {
            return true;
        }
        const bool hasPending = pendingAttributes != INVALID_FILE_ATTRIBUTES;
        if (hasPending) {
            if (!read_marker(pending, marker)) {
                return false;
            }
            priorMask = marker.priorMask;
        } else {
            for (std::size_t index = 0; index < resources.size(); ++index) {
                if (GetFileAttributesW(resources[index].backup.c_str())
                    != INVALID_FILE_ATTRIBUTES) {
                    priorMask |= static_cast<std::uint8_t>(1U << index);
                }
            }
        }
        for (std::size_t index = resources.size(); index != 0; --index) {
            Resource& resource = resources[index - 1U];
            const bool hadPrior = (priorMask & (1U << (index - 1U))) != 0;
            const bool hasBackup =
                GetFileAttributesW(resource.backup.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (hasBackup) {
                if (!remove_final(resource)
                    || !default_move(nullptr, resource.backup.c_str(), resource.final)) {
                    return false;
                }
            } else if (hasPending) {
                if (!hadPrior) {
                    if (!remove_final(resource)) {
                        return false;
                    }
                } else {
                    bool exists = false;
                    if (!final_state(resource.final, resource.directory, exists) || !exists) {
                        return false;
                    }
                }
            }
        }
        return pendingAttributes == INVALID_FILE_ATTRIBUTES
               || DeleteFileW(pending.c_str()) != FALSE;
    } catch (...) {
        return false;
    }
}

/** Removes bounded writer-owned stage siblings left by an interrupted process. */
[[nodiscard]] bool discard_stale_stages(const std::wstring& parent) noexcept {
    WIN32_FIND_DATAW entry{};
    const std::wstring search = parent + L"\\.activity-sdk-stage.*";
    const HANDLE find = FindFirstFileW(search.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool complete = true;
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring path = parent + L"\\" + std::wstring(name);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            || !tree_publication::discard(path.c_str())) {
            complete = false;
        }
    } while (FindNextFileW(find, &entry) != FALSE);
    const DWORD endError = GetLastError();
    return FindClose(find) != FALSE && endError == ERROR_NO_MORE_FILES && complete;
}

} // namespace

/** Returns one stable bounded reason for a live publication result. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::ready:
        return "ready";
    case Status::invalidInput:
        return "invalid_input";
    case Status::stageAllocation:
        return "stage_allocation";
    case Status::backupCollision:
        return "backup_collision";
    case Status::backupFailure:
        return "backup_failure";
    case Status::commitFailure:
        return "commit_failure";
    case Status::finalizeFailure:
        return "finalize_failure";
    case Status::rollbackFailure:
        return "rollback_failure";
    }
    return "invalid_input";
}

/** Recovers any bounded prior journal, removes stale stages, and allocates one isolated sibling. */
Status allocate(const wchar_t* finalPackPath, Stage& output) noexcept {
    output = {};
    if (finalPackPath == nullptr || finalPackPath[0] == L'\0') {
        return Status::invalidInput;
    }
    std::wstring normalizedPack;
    if (!full_path(finalPackPath, normalizedPack)) {
        return Status::invalidInput;
    }
    const std::wstring_view pack(normalizedPack);
    std::wstring_view parent;
    std::wstring_view leaf;
    if (!split(pack, parent, leaf) || !same_text(leaf, L"activity_sdk.pack")) {
        return Status::invalidInput;
    }
    try {
        const std::wstring lexicalParent(parent);
        std::wstring parentPath;
        if (!canonical_directory(lexicalParent.c_str(), parentPath)) {
            return Status::invalidInput;
        }
        const std::wstring finalSdkDirectory = parentPath + L"\\sdk";
        const std::wstring finalCatalogPath = finalSdkDirectory + L"\\catalog.bin";
        if (!ordinary_directory(parentPath.c_str())
            || !ordinary_directory(finalSdkDirectory.c_str())
            || !recover(finalSdkDirectory.c_str(), normalizedPack.c_str(), finalCatalogPath.c_str())
            || !discard_stale_stages(parentPath)) {
            return Status::invalidInput;
        }
        std::array<wchar_t, 80> suffix{};
        for (std::size_t attempt = 0; attempt < kAllocationAttempts; ++attempt) {
            const DWORD sequence = static_cast<DWORD>(InterlockedIncrement(&g_sequence));
            const int length = std::swprintf(suffix.data(),
                                             suffix.size(),
                                             kStageSuffix.data(),
                                             GetCurrentProcessId(),
                                             GetCurrentThreadId(),
                                             sequence);
            if (length <= 0 || static_cast<std::size_t>(length) >= suffix.size()) {
                return Status::stageAllocation;
            }
            output.root = parentPath;
            output.root.append(suffix.data(), static_cast<std::size_t>(length));
            if (CreateDirectoryW(output.root.c_str(), nullptr) == FALSE) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                    output = {};
                    continue;
                }
                output = {};
                return Status::stageAllocation;
            }
            output.sdkDirectory = output.root + L"\\sdk";
            output.packPath = output.root + L"\\activity_sdk.pack";
            output.catalogPath = output.sdkDirectory + L"\\catalog.bin";
            output.luaDirectory = output.sdkDirectory + L"\\lua";
            if (!ensure_directory(output.sdkDirectory)) {
                (void)discard(output);
                output = {};
                return Status::stageAllocation;
            }
            return Status::ready;
        }
    } catch (...) {
        (void)discard(output);
        output = {};
        return Status::stageAllocation;
    }
    return Status::stageAllocation;
}

/** Commits Lua, pack, and catalog in order while retaining rollback state through finalization. */
Status publish(const Stage& stage,
               const wchar_t* finalSdkDirectory,
               const wchar_t* finalPackPath,
               const wchar_t* finalCatalogPath,
               MoveOperation move,
               void* moveContext,
               FinalizeOperation finalize,
               void* finalizeContext) noexcept {
    if (stage.root.empty() || stage.sdkDirectory.empty() || stage.packPath.empty()
        || stage.catalogPath.empty() || stage.luaDirectory.empty() || finalSdkDirectory == nullptr
        || finalPackPath == nullptr || finalCatalogPath == nullptr) {
        return Status::invalidInput;
    }
    std::wstring canonicalStageRoot;
    std::wstring canonicalStageSdk;
    std::wstring canonicalStageLua;
    std::wstring canonicalFinalSdk;
    std::wstring canonicalStagePack;
    std::wstring canonicalStageCatalog;
    std::wstring canonicalFinalPack;
    std::wstring canonicalFinalCatalog;
    if (!canonical_directory(stage.root.c_str(), canonicalStageRoot)
        || !canonical_directory(stage.sdkDirectory.c_str(), canonicalStageSdk)
        || !canonical_directory(stage.luaDirectory.c_str(), canonicalStageLua)
        || !canonical_directory(finalSdkDirectory, canonicalFinalSdk)
        || !full_path(stage.packPath.c_str(), canonicalStagePack)
        || !full_path(stage.catalogPath.c_str(), canonicalStageCatalog)
        || !full_path(finalPackPath, canonicalFinalPack)
        || !full_path(finalCatalogPath, canonicalFinalCatalog)
        || !ordinary_file(canonicalStagePack.c_str())
        || !ordinary_file(canonicalStageCatalog.c_str())) {
        return Status::invalidInput;
    }
    const std::wstring expectedStageSdk = canonicalStageRoot + L"\\sdk";
    const std::wstring expectedStageLua = expectedStageSdk + L"\\lua";
    const std::wstring expectedStagePack = canonicalStageRoot + L"\\activity_sdk.pack";
    const std::wstring expectedStageCatalog = expectedStageSdk + L"\\catalog.bin";
    const std::wstring expectedFinalCatalog = canonicalFinalSdk + L"\\catalog.bin";
    std::wstring_view stageParent;
    std::wstring_view stageLeaf;
    std::wstring_view finalParent;
    std::wstring_view finalLeaf;
    if (!split(canonicalStageRoot, stageParent, stageLeaf)
        || !split(canonicalFinalPack, finalParent, finalLeaf)) {
        return Status::invalidInput;
    }
    const std::wstring expectedFinalSdk = std::wstring(finalParent) + L"\\sdk";
    if (!same_text(stageParent, finalParent) || !starts_text(stageLeaf, L".activity-sdk-stage.")
        || !same_text(canonicalStageSdk, expectedStageSdk)
        || !same_text(canonicalStageLua, expectedStageLua)
        || !same_text(canonicalStagePack, expectedStagePack)
        || !same_text(canonicalStageCatalog, expectedStageCatalog)
        || !same_text(canonicalFinalSdk, expectedFinalSdk)
        || !same_text(canonicalFinalCatalog, expectedFinalCatalog)
        || !same_text(finalLeaf, L"activity_sdk.pack")) {
        return Status::invalidInput;
    }
    std::wstring finalLua;
    try {
        finalLua = canonicalFinalSdk;
        finalLua.append(L"\\lua");
    } catch (...) {
        return Status::invalidInput;
    }
    std::array<Resource, 3> resources{
        Resource{canonicalStageLua.c_str(), finalLua.c_str(), {}, true},
        Resource{canonicalStagePack.c_str(), canonicalFinalPack.c_str(), {}, false},
        Resource{canonicalStageCatalog.c_str(), canonicalFinalCatalog.c_str(), {}, false},
    };
    std::wstring pendingMarker;
    std::wstring committedMarker;
    try {
        pendingMarker = canonicalFinalSdk;
        pendingMarker.append(kPendingMarkerSuffix);
        committedMarker = canonicalFinalSdk;
        committedMarker.append(kCommittedMarkerSuffix);
    } catch (...) {
        return Status::invalidInput;
    }
    std::uint8_t priorMask = 0;
    for (Resource& resource : resources) {
        if (!final_state(resource.final, resource.directory, resource.hadFinal)
            || !backup_path(resource.final, resource.backup)) {
            return Status::invalidInput;
        }
        if (GetFileAttributesW(resource.backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return Status::backupCollision;
        }
    }
    for (std::size_t index = 0; index < resources.size(); ++index) {
        if (resources[index].hadFinal) {
            priorMask |= static_cast<std::uint8_t>(1U << index);
        }
    }
    if (GetFileAttributesW(pendingMarker.c_str()) != INVALID_FILE_ATTRIBUTES
        || GetFileAttributesW(committedMarker.c_str()) != INVALID_FILE_ATTRIBUTES
        || !write_marker(pendingMarker, priorMask)) {
        return Status::backupCollision;
    }
    const MoveOperation rename = move != nullptr ? move : &default_move;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const Status moved = advance(resources, index, rename, moveContext);
        if (moved != Status::ready) {
            if (moved != Status::rollbackFailure) {
                (void)DeleteFileW(pendingMarker.c_str());
            }
            return moved;
        }
    }
    if (finalize != nullptr && !finalize(finalizeContext)) {
        const bool restored = rollback(resources, resources.size(), rename, moveContext);
        if (restored) {
            (void)DeleteFileW(pendingMarker.c_str());
        }
        return restored ? Status::finalizeFailure : Status::rollbackFailure;
    }
    if (!rename(moveContext, pendingMarker.c_str(), committedMarker.c_str())) {
        bool restored = rollback(resources, resources.size(), rename, moveContext);
        if (finalize != nullptr) {
            restored = finalize(finalizeContext) && restored;
        }
        if (restored) {
            (void)DeleteFileW(pendingMarker.c_str());
        }
        return restored ? Status::commitFailure : Status::rollbackFailure;
    }
    if (discard_backups(resources)) {
        (void)DeleteFileW(committedMarker.c_str());
    }
    (void)discard(stage);
    return Status::ready;
}

bool discard(const Stage& stage) noexcept {
    return stage.root.empty() || tree_publication::discard(stage.root.c_str());
}

} // namespace sunrise::client::content::activity::sdk_generation::live_publication
