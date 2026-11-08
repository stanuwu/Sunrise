#include <Windows.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <memory>
#include <string>

#include "../../../middleware/content/packages/named_tags.h"
#include "../../content_manifest/scanner/package/content_manifest_package_name.h"
#include "../discovery/mod_directory_listing.h"
#include "../problem/mod_problem.h"
#include "internal.h"

namespace sunrise::state::mods::packages::internal {
namespace {

namespace name = content_manifest::scanner::package;
namespace named_tags = middleware::content::packages::named_tags;

/** Lists every entry of one directory. */
constexpr std::wstring_view kWildcard = L"*";
/** Leaf characters quoted in a message, so one bad name cannot fill it. */
constexpr int kQuotedLeafLimit = 80;
/** One flag per possible generation of one package id. */
constexpr std::size_t kGenerationCount = 256;
/** Wide characters below this are ASCII and quote as themselves. */
constexpr wchar_t kAsciiLimit = 0x80;

/** Generations of one new package id found in one mod. */
struct NewPackage final {
    std::uint16_t packageId{};
    std::bitset<kGenerationCount> generations{};
    Base newest{};
};

/** What one mod stages; committed only when the mod stays loaded. */
struct Staged final {
    std::vector<OverlayFile> files;
    std::vector<Claim> claims;
    std::vector<AddedFile> added;
    std::vector<table_merge::Sidecar> sidecars;
    /** Older patches of a chain, served from files as parents of the claimed newest one. */
    std::size_t chainParents{};
};

/**
 * Reads the whole merge data file next to one package file.
 * @param present Receives false when there is no such file.
 * @return ok, or why a present file cannot be used.
 */
[[nodiscard]] table_merge::Status
read_sidecar(const std::wstring& packagePath, std::vector<std::byte>& bytes, bool& present) {
    const std::wstring path =
        packagePath.substr(0, packagePath.size() - kPackageFileExtension.size())
            .append(table_merge::kSidecarFileExtension);
    const FileRead read = read_whole_file(path, table_merge::kSidecarByteLimit, bytes);
    present = read != FileRead::missing;
    switch (read) {
    case FileRead::ok:
        return table_merge::Status::ok;
    case FileRead::tooLarge:
        return table_merge::Status::capacity;
    default:
        return table_merge::Status::format;
    }
}

/**
 * Parses the merge data of one staged package file. Bad data is a warning, never a refusal.
 * The data binds to the package header digest, so it cannot describe another build.
 * @param digest The package header's metadata digest.
 * @return The index of the parsed data in `sidecars`, or kNoSidecar.
 */
[[nodiscard]] std::size_t load_sidecar(Record& record,
                                       const std::wstring& packagePath,
                                       std::uint16_t packageId,
                                       std::uint32_t patchId,
                                       const merge::MetadataDigest& digest,
                                       std::vector<table_merge::Sidecar>& sidecars) {
    std::vector<std::byte> bytes;
    bool present = false;
    table_merge::Status status = read_sidecar(packagePath, bytes, present);
    if (!present) {
        return kNoSidecar;
    }
    table_merge::Sidecar parsed;
    if (status == table_merge::Status::ok) {
        status = table_merge::parse(
            std::move(bytes), packageId, static_cast<std::uint16_t>(patchId), digest, parsed);
    }
    if (status != table_merge::Status::ok) {
        warn_ignored(record, packageId, table_merge::status_text(status));
        return kNoSidecar;
    }
    sidecars.push_back(std::move(parsed));
    return sidecars.size() - 1;
}

/** @return True when the leaf ends in `.pkg`, in any case. */
[[nodiscard]] bool is_package_file(std::wstring_view leaf) noexcept {
    if (leaf.size() <= kPackageFileExtension.size()) {
        return false;
    }
    const std::wstring_view tail = leaf.substr(leaf.size() - kPackageFileExtension.size());
    return CompareStringOrdinal(tail.data(),
                                static_cast<int>(tail.size()),
                                kPackageFileExtension.data(),
                                static_cast<int>(kPackageFileExtension.size()),
                                TRUE)
           == CSTR_EQUAL;
}

/** @return True when `path` names an existing directory. */
[[nodiscard]] bool directory_exists(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Lists the package files of one mod subfolder, sorted by leaf. A missing folder is empty. */
[[nodiscard]] bool list(const std::wstring& directory, std::vector<Listed>& output) {
    output.clear();
    const std::wstring search = join(directory, kWildcard);
    WIN32_FIND_DATAW entry{};
    const HANDLE listing = FindFirstFileW(search.c_str(), &entry);
    if (listing == INVALID_HANDLE_VALUE) {
        return directory_listing::empty(directory, GetLastError());
    }
    directory_listing::Cursor cursor(listing);
    do {
        const std::wstring_view leaf(entry.cFileName);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !is_package_file(leaf)) {
            continue;
        }
        Listed& listed = output.emplace_back();
        listed.path = join(directory, leaf);
        listed.leaf.reserve(leaf.size());
        for (const wchar_t value : leaf) {
            listed.leaf.push_back(value < kAsciiLimit ? static_cast<char>(value) : '?');
        }
        listed.status = name::parse_file_name(leaf, listed.parsed);
    } while (FindNextFileW(listing, &entry) != FALSE);
    if (!cursor.finish(GetLastError())) {
        output.clear();
        return false;
    }
    std::sort(output.begin(), output.end(), [](const Listed& first, const Listed& second) {
        return first.leaf < second.leaf;
    });
    return true;
}

/** @return The record holding `id`, or kNoRecord. */
[[nodiscard]] std::size_t find_record(std::span<const Record> records, std::string_view id) {
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index].manifestRead && records[index].manifest.id.view() == id) {
            return index;
        }
    }
    return kNoRecord;
}

/**
 * Refuses a mod whose dependency was refused while its own files were checked.
 * @return True when every dependency is still loaded.
 */
[[nodiscard]] bool dependencies_loaded(std::span<Record> records, Record& record) {
    for (std::size_t entry = 0; entry < record.manifest.dependencyCount; ++entry) {
        const Id& wanted = record.manifest.dependencies[entry].id;
        const std::size_t found = find_record(records, wanted.view());
        if (found == kNoRecord || records[found].status != Status::loaded) {
            problem::refuse(record,
                            Reason::dependencyNotLoaded,
                            "Needs %s, which did not load.",
                            wanted.bytes.data());
            return false;
        }
    }
    return true;
}

/** @return The printf precision that quotes at most `kQuotedLeafLimit` characters. */
[[nodiscard]] int quoted(const std::string& leaf) noexcept {
    return static_cast<int>((std::min)(leaf.size(), std::size_t{kQuotedLeafLimit}));
}

/** Accepts every entry; the walk only proves the names table parses. */
[[nodiscard]] bool accept_name(void*, const named_tags::Entry&) noexcept {
    return true;
}

/**
 * Refuses a file whose names table does not parse. The server's name walk reads every served file.
 * @return True when the names table parses.
 */
[[nodiscard]] bool check_names(Record& record, const Listed& file, Reason reason) {
    named_tags::Result names{};
    if (named_tags::extract_file(file.path.c_str(), &accept_name, nullptr, names)) {
        return true;
    }
    problem::refuse(record,
                    reason,
                    "%.*s: its package names could not be read.",
                    quoted(file.leaf),
                    file.leaf.c_str());
    return false;
}

/**
 * Refuses a listed file whose name is not a patchable package name.
 * @return True when the name is usable.
 */
[[nodiscard]] bool check_name(Record& record, const Listed& file, const char* excludedText) {
    if (file.status == name::NameStatus::recognized) {
        return true;
    }
    if (file.status == name::NameStatus::excluded) {
        problem::refuse(record,
                        Reason::packageInvalid,
                        "%.*s: %s",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        excludedText);
        return false;
    }
    problem::refuse(record,
                    Reason::packageInvalid,
                    "%.*s: the name must be w64_<family>_<4-digit hex id>_<patch>.pkg.",
                    quoted(file.leaf),
                    file.leaf.c_str());
    return false;
}

/** @return The manifest row for one parsed name. */
[[nodiscard]] content_manifest::Row row_for(const name::ParsedName& parsed,
                                            std::uint64_t buildSignature) noexcept {
    content_manifest::Row row{};
    std::copy_n(parsed.stem.begin(), parsed.stemLength, row.name.begin());
    row.nameLength = parsed.stemLength;
    row.packageId = parsed.packageId;
    row.buildSignature = buildSignature;
    return row;
}

/**
 * Checks one new package file and adds it to its id's group and to the staged files.
 * @return False when the file refused the mod.
 */
[[nodiscard]] bool stage_new_file(Work& work,
                                  Record& record,
                                  const Listed& file,
                                  std::vector<NewPackage>& packages,
                                  Staged& staged) {
    if (!check_name(record, file, "startup _unp1_ packages cannot be replaced by a mod.")) {
        return false;
    }
    const std::uint16_t packageId = file.parsed.packageId;
    merge::Identity identity{};
    const merge::Status status = merge::identify(file.path.c_str(), identity);
    if (status != merge::Status::ok) {
        problem::refuse(record,
                        Reason::packageInvalid,
                        "%.*s: %s.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        merge::status_text(status));
        return false;
    }
    if (!check_names(record, file, Reason::packageInvalid)) {
        return false;
    }
    if (identity.packageId != packageId || identity.patchId != file.parsed.patchIndex) {
        problem::refuse(record,
                        Reason::packageInvalid,
                        "%.*s: its header names package 0x%04X generation %u.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        identity.packageId,
                        identity.patchId);
        return false;
    }
    if (packageId > kLastOwnPackageId) {
        problem::refuse(record,
                        Reason::packageInvalid,
                        "%.*s: new package ids must be 0x%04X to 0x%04X.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        kFirstOwnPackageId,
                        kLastOwnPackageId);
        return false;
    }
    const Base& taken = work.bases[packageId];
    if (taken.present) {
        if (taken.owner == kInstalledOwner) {
            problem::refuse(
                record, Reason::packageConflict, "Package 0x%04X is already installed.", packageId);
        } else {
            problem::refuse(record,
                            Reason::packageConflict,
                            "Package 0x%04X is also added by %s.",
                            packageId,
                            work.records[taken.owner].manifest.id.bytes.data());
        }
        return false;
    }
    if (work.reserved.test(packageId)) {
        problem::refuse(record,
                        Reason::packageConflict,
                        "Package 0x%04X is used by a startup package.",
                        packageId);
        return false;
    }
    OverlayFile& overlay = staged.files.emplace_back();
    overlay.row = row_for(file.parsed, identity.buildSignature);
    if (!client_path(work, file.path, overlay)) {
        problem::refuse(record,
                        Reason::folderPath,
                        "%.*s: the path must be ASCII, inside the game folder, and under %zu "
                        "characters.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        kClientPathCapacity);
        return false;
    }
    auto group =
        std::find_if(packages.begin(), packages.end(), [packageId](const NewPackage& entry) {
            return entry.packageId == packageId;
        });
    if (group == packages.end()) {
        group = packages.insert(packages.end(), NewPackage{packageId});
    }
    group->generations.set(file.parsed.patchIndex);
    if (!group->newest.present || file.parsed.patchIndex > group->newest.patch) {
        group->newest =
            Base{overlay.row, file.parsed.identityLength, file.parsed.patchIndex, 0, true};
    }
    AddedFile& added = staged.added.emplace_back();
    added.path = file.path;
    added.packageId = packageId;
    added.patch = file.parsed.patchIndex;
    added.sidecar = load_sidecar(
        record, file.path, packageId, added.patch, identity.metadataDigest, staged.sidecars);
    return true;
}

/**
 * Checks that each new package id ships every generation from 0 and one family name.
 * @return False when a group refused the mod.
 */
[[nodiscard]] bool check_generations(Record& record,
                                     const std::vector<NewPackage>& packages,
                                     std::span<const OverlayFile> staged) {
    for (const NewPackage& package : packages) {
        const std::size_t expected = std::size_t{package.newest.patch} + 1;
        if (package.generations.count() != expected) {
            problem::refuse(record,
                            Reason::packageInvalid,
                            "Package 0x%04X must ship every generation from 0 to %u.",
                            package.packageId,
                            package.newest.patch);
            return false;
        }
        const std::string_view identity(package.newest.newest.name.data(),
                                        package.newest.identityLength);
        for (const OverlayFile& file : staged) {
            const std::string_view stem(file.row.name.data(), file.row.nameLength);
            if (file.row.packageId == package.packageId && !stem.starts_with(identity)) {
                problem::refuse(record,
                                Reason::packageInvalid,
                                "Package 0x%04X uses more than one file name family.",
                                package.packageId);
                return false;
            }
        }
    }
    return true;
}

/**
 * Checks one patch file against the newest generation it must extend.
 * @return False when the patch refused the mod.
 */
[[nodiscard]] bool
stage_patch(Work& work, std::size_t recordIndex, const Listed& file, Staged& staged) {
    Record& record = work.records[recordIndex];
    if (!check_name(record, file, "startup _unp1_ packages cannot be patched by a mod.")) {
        return false;
    }
    const std::uint16_t packageId = file.parsed.packageId;
    const Base& base = work.bases[packageId];
    if (!base.present) {
        problem::refuse(record,
                        Reason::patchTarget,
                        "%.*s: package 0x%04X is not installed.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        packageId);
        return false;
    }
    // A chain cannot be merged with another mod's patch of the same package, in either order.
    const auto earlier =
        std::find_if(work.claims.begin(), work.claims.end(), [packageId](const Claim& claim) {
            return claim.file.row.packageId == packageId;
        });
    if (earlier != work.claims.end() && earlier->chained) {
        problem::refuse(
            record,
            Reason::patchInvalid,
            "Package 0x%04X is patched in several generations by %s, so no other mod may patch it.",
            packageId,
            work.records[earlier->record].manifest.id.bytes.data());
        return false;
    }
    // Patch files arrive sorted by package and generation, so a chain's previous link is the last
    // claim.
    const auto previous =
        std::find_if(staged.claims.begin(), staged.claims.end(), [packageId](const Claim& claim) {
            return claim.file.row.packageId == packageId;
        });
    const bool chained = previous != staged.claims.end();
    const std::uint32_t expected = chained ? previous->generation + 1 : base.patch + 1;
    const std::string_view baseIdentity(base.newest.name.data(), base.identityLength);
    const std::string_view identity(file.parsed.stem.data(), file.parsed.identityLength);
    if (identity != baseIdentity || file.parsed.patchIndex != expected) {
        problem::refuse(record,
                        Reason::patchInvalid,
                        "%.*s: the next generation of package 0x%04X is %.*s_%u.pkg.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        packageId,
                        static_cast<int>(baseIdentity.size()),
                        baseIdentity.data(),
                        expected);
        return false;
    }
    if (chained && earlier != work.claims.end()) {
        problem::refuse(
            record,
            Reason::patchInvalid,
            "Package 0x%04X is also patched by %s, so it cannot be patched in several generations.",
            packageId,
            work.records[earlier->record].manifest.id.bytes.data());
        return false;
    }
    merge::PatchSummary summary{};
    const merge::Status status =
        merge::inspect(chained ? previous->path.c_str() : base_path(work, base).c_str(),
                       file.path.c_str(),
                       summary);
    if (status != merge::Status::ok) {
        problem::refuse(record,
                        Reason::patchInvalid,
                        "%.*s: %s.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        merge::status_text(status));
        return false;
    }
    if (!check_names(record, file, Reason::patchInvalid)) {
        return false;
    }
    std::bitset<merge::kEntryLimit> changed = summary.changedEntries;
    if (chained) {
        // The older link stays served as a parent; the claim moves to the newer one.
        changed |= previous->changed;
        staged.files.push_back(previous->file);
        ++staged.chainParents;
        staged.claims.erase(previous);
    }
    Claim& claim = staged.claims.emplace_back();
    claim.record = recordIndex;
    claim.path = file.path;
    claim.file.row = row_for(file.parsed, summary.buildSignature);
    claim.changed = changed;
    claim.generation = file.parsed.patchIndex;
    claim.chained = chained;
    if (!client_path(work, file.path, claim.file)) {
        problem::refuse(record,
                        Reason::folderPath,
                        "%.*s: the path must be ASCII, inside the game folder, and under %zu "
                        "characters.",
                        quoted(file.leaf),
                        file.leaf.c_str(),
                        kClientPathCapacity);
        return false;
    }
    claim.sidecar = load_sidecar(record,
                                 file.path,
                                 packageId,
                                 file.parsed.patchIndex,
                                 summary.metadataDigest,
                                 staged.sidecars);
    return true;
}

/** Moves one mod's staged files and merge data into the stage. */
void commit(Work& work,
            std::size_t recordIndex,
            std::vector<NewPackage>& packages,
            Staged& staged) {
    for (NewPackage& package : packages) {
        package.newest.owner = recordIndex;
        work.bases[package.packageId] = package.newest;
    }
    const std::size_t firstSidecar = work.sidecars.size();
    const auto rebase = [firstSidecar](std::size_t index) {
        return index == kNoSidecar ? kNoSidecar : index + firstSidecar;
    };
    for (table_merge::Sidecar& sidecar : staged.sidecars) {
        work.sidecars.push_back(std::move(sidecar));
    }
    work.files.insert(work.files.end(), staged.files.begin(), staged.files.end());
    work.rawParents.insert(work.rawParents.end(), staged.files.begin(), staged.files.end());
    for (AddedFile& added : staged.added) {
        added.record = recordIndex;
        added.sidecar = rebase(added.sidecar);
        work.added.push_back(std::move(added));
    }
    for (Claim& claim : staged.claims) {
        claim.sidecar = rebase(claim.sidecar);
        work.claims.push_back(std::move(claim));
    }
}

} // namespace

/** A file that exists but cannot be opened or read is failed, not missing. */
FileRead
read_whole_file(const std::wstring& path, std::size_t limit, std::vector<std::byte>& bytes) {
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileRead::missing
                                                                              : FileRead::failed;
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> owner(file, &CloseHandle);
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0) {
        return FileRead::failed;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > limit
        || static_cast<std::uint64_t>(size.QuadPart) > (std::numeric_limits<DWORD>::max)()) {
        return FileRead::tooLarge;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD count = 0;
    return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != FALSE
                   && count == bytes.size()
               ? FileRead::ok
               : FileRead::failed;
}

/** Checks one loaded mod's packages and patches, and commits them if the mod stays loaded. */
void stage_mod(Work& work, std::size_t recordIndex) {
    Record& record = work.records[recordIndex];
    // A mod refused by an earlier pass stays out.
    if (record.status != Status::loaded || !dependencies_loaded(work.records, record)) {
        return;
    }
    const std::wstring directory = mod_directory(work, record);
    record.hasSdk = directory_exists(join(directory, kSdkFolder));
    record.hasScripts = directory_exists(join(directory, kScriptsFolder));

    std::vector<NewPackage> packages;
    Staged staged;
    std::vector<Listed> files;
    std::vector<Listed> patchFiles;
    if (!list(join(directory, kPackagesFolder), files)) {
        problem::refuse(record, Reason::packageInvalid, "The packages folder could not be listed.");
        return;
    }
    if (!list(join(directory, kPatchesFolder), patchFiles)) {
        problem::refuse(record, Reason::packageInvalid, "The patches folder could not be listed.");
        return;
    }
    if (!relocate(work, recordIndex, directory, files, patchFiles)) {
        return;
    }
    for (const Listed& file : files) {
        if (!stage_new_file(work, record, file, packages, staged)) {
            return;
        }
    }
    if (!check_generations(record, packages, staged.files)) {
        return;
    }
    // By package, then generation, so each link of a chain follows the one it extends.
    std::stable_sort(
        patchFiles.begin(), patchFiles.end(), [](const Listed& first, const Listed& second) {
            return first.parsed.packageId != second.parsed.packageId
                       ? first.parsed.packageId < second.parsed.packageId
                       : first.parsed.patchIndex < second.parsed.patchIndex;
        });
    for (const Listed& file : patchFiles) {
        if (!stage_patch(work, recordIndex, file, staged)) {
            return;
        }
    }
    std::vector<Bound> bindings;
    if (!admit_bindings(work, recordIndex, staged.files, staged.claims, bindings)) {
        return;
    }
    // A patched id serves one file however many mods patch it, so claims bound the count.
    if (work.files.size() + work.claims.size() + staged.files.size() + staged.claims.size()
        > work.fileCapacity) {
        problem::refuse(record,
                        Reason::capacity,
                        "Mods add more than %zu package files together.",
                        work.fileCapacity);
        return;
    }
    record.newPackageFiles = static_cast<std::uint32_t>(staged.files.size() - staged.chainParents);
    record.patchFiles = static_cast<std::uint32_t>(staged.claims.size() + staged.chainParents);
    work.bindings.insert(work.bindings.end(), bindings.begin(), bindings.end());
    commit(work, recordIndex, packages, staged);
}

} // namespace sunrise::state::mods::packages::internal
