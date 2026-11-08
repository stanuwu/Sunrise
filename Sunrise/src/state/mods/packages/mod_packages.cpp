#include "mod_packages.h"

#include <Windows.h>

#include <algorithm>
#include <bitset>
#include <string>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../content_manifest/scanner/package/content_manifest_package_name.h"
#include "../discovery/mod_directory_listing.h"
#include "../problem/mod_problem.h"
#include "internal.h"

namespace sunrise::state::mods::packages {
namespace internal {
namespace {

namespace layout = middleware::content::packages::reader::layout;

/** Characters below this are control characters the client path cannot carry. */
constexpr wchar_t kFirstPrintable = 0x20;
/** Characters from here on are not ASCII. */
constexpr wchar_t kAsciiLimit = 0x80;
/** Lowercase hex digits for cache file names. */
constexpr std::string_view kHexDigits = "0123456789abcdef";
/** Each key byte prints as two 4-bit digits. */
constexpr unsigned kNibbleBits = 4;
constexpr unsigned kNibbleMask = 0x0F;

} // namespace

/** @return `directory\leaf`. */
std::wstring join(std::wstring_view directory, std::wstring_view leaf) {
    std::wstring output;
    output.reserve(directory.size() + kSeparator.size() + leaf.size());
    output.append(directory).append(kSeparator).append(leaf);
    return output;
}

/** @return The ASCII stem of one row as a wide string. */
std::wstring widen(std::string_view text) {
    return {text.begin(), text.end()};
}

/** @return The absolute folder of one mod. */
std::wstring mod_directory(const Work& work, const Record& record) {
    return join(work.paths.modsDirectory, widen(record.folder.view()));
}

/** @return The file of one package id's newest generation. */
std::wstring base_path(const Work& work, const Base& base) {
    std::wstring leaf = widen({base.newest.name.data(), base.newest.nameLength});
    leaf += kPackageFileExtension;
    if (base.owner == kInstalledOwner) {
        return join(work.paths.packagesDirectory, leaf);
    }
    return join(join(mod_directory(work, work.records[base.owner]), kPackagesFolder), leaf);
}

/** @return `<stem><infix><64 hex digits>.pkg`, naming one cache file by its key. */
std::wstring cache_leaf_name(std::string_view stem,
                             std::wstring_view infix,
                             const content_manifest::Fingerprint& digest) {
    std::wstring leaf = widen(stem);
    leaf += infix;
    for (const std::byte byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        leaf.push_back(static_cast<wchar_t>(kHexDigits[value >> kNibbleBits]));
        leaf.push_back(static_cast<wchar_t>(kHexDigits[value & kNibbleMask]));
    }
    leaf += kPackageFileExtension;
    return leaf;
}

/** @return The package id of an entry tag. */
std::uint16_t package_id(std::uint32_t tag) noexcept {
    return static_cast<std::uint16_t>(layout::package_of(tag));
}

/** Warns a mod that the merge data of one of its package files was not used. */
void warn_ignored(Record& record, std::uint16_t packageId, const char* reason) {
    problem::warn(record,
                  Reason::mergeFailed,
                  "Merge data for package 0x%04X was ignored: %s.",
                  packageId,
                  reason);
}

/** Sets a file's client path relative to the game directory. */
bool client_path(const Work& work, std::wstring_view absolute, OverlayFile& file) {
    const std::wstring_view game = work.paths.gameDirectory;
    if (absolute.size() <= game.size() + kSeparator.size()
        || absolute.substr(game.size(), kSeparator.size()) != kSeparator
        || CompareStringOrdinal(absolute.data(),
                                static_cast<int>(game.size()),
                                game.data(),
                                static_cast<int>(game.size()),
                                TRUE)
               != CSTR_EQUAL) {
        return false;
    }
    const std::wstring_view relative = absolute.substr(game.size() + kSeparator.size());
    if (relative.size() >= file.clientPath.size()) {
        return false;
    }
    for (std::size_t index = 0; index < relative.size(); ++index) {
        const wchar_t value = relative[index];
        if (value < kFirstPrintable || value >= kAsciiLimit) {
            return false;
        }
        file.clientPath[index] = static_cast<char>(value);
    }
    file.clientPath[relative.size()] = '\0';
    file.clientPathLength = relative.size();
    return true;
}

} // namespace internal

namespace {

/** Seeds the base table with the newest installed generation of every package id. */
void seed_installed(internal::Work& work, std::span<const content_manifest::Row> installed) {
    for (const content_manifest::Row& row : installed) {
        content_manifest::scanner::package::ParsedName parsed{};
        if (!content_manifest::scanner::package::parse_stem({row.name.data(), row.nameLength},
                                                            parsed)) {
            continue;
        }
        internal::Base& base = work.bases[row.packageId];
        if (!base.present || parsed.patchIndex > base.patch) {
            base = internal::Base{
                row, parsed.identityLength, parsed.patchIndex, internal::kInstalledOwner, true};
        }
    }
}

/**
 * Reads the ids of the startup `_unp1_` packages. The manifest leaves them out, so a mod could
 * otherwise add a package with one of their ids.
 */
[[nodiscard]] std::bitset<internal::kPackageIdCount> reserved_ids(const Paths& paths) {
    std::bitset<internal::kPackageIdCount> reserved{};
    const std::wstring search = internal::join(paths.packagesDirectory, L"*_unp1_*.pkg");
    WIN32_FIND_DATAW entry{};
    const HANDLE listing = FindFirstFileW(search.c_str(), &entry);
    if (listing == INVALID_HANDLE_VALUE) {
        return reserved;
    }
    directory_listing::Cursor cursor(listing);
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring path = internal::join(paths.packagesDirectory, entry.cFileName);
        internal::merge::Identity identity{};
        if (internal::merge::identify(path.c_str(), identity) != internal::merge::Status::ok) {
            std::string leaf;
            for (const wchar_t* value = entry.cFileName; *value != L'\0'; ++value) {
                leaf.push_back(*value < internal::kAsciiLimit ? static_cast<char>(*value) : '?');
            }
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=mods stage=reserve file=%s result=fail",
                              leaf.c_str());
            continue;
        }
        if (identity.packageId < reserved.size()) {
            reserved.set(identity.packageId);
        }
    } while (FindNextFileW(listing, &entry) != FALSE);
    (void)cursor.finish(GetLastError());
    return reserved;
}

/** @return True when the first file's stem sorts before the second's. */
[[nodiscard]] bool stem_less(const OverlayFile& first, const OverlayFile& second) noexcept {
    return std::string_view(first.row.name.data(), first.row.nameLength)
           < std::string_view(second.row.name.data(), second.row.nameLength);
}

/**
 * Runs the package stage once over the current records.
 * @param reserved Package ids no mod may add.
 * @param result Receives the counts; complete only when the files were copied out.
 * @return The record whose refusal stopped the stage, or kNoRecord.
 */
[[nodiscard]] std::size_t run_pass(const Paths& paths,
                                   std::span<const content_manifest::Row> installed,
                                   const std::bitset<internal::kPackageIdCount>& reserved,
                                   std::span<Record> records,
                                   std::span<const std::size_t> order,
                                   std::span<OverlayFile> files,
                                   Result& result) {
    result = {};
    internal::Work work{paths, records};
    work.fileCapacity = files.size();
    work.reserved = reserved;
    work.bases.resize(internal::kPackageIdCount);
    seed_installed(work, installed);
    internal::load_remaps(work);
    for (const std::size_t record : order) {
        internal::stage_mod(work, record);
    }
    result.mergedPackages = internal::serve_claims(work);
    if (work.complete && internal::rewrite_packages(work)) {
        internal::warn_overlaps(work);
        std::sort(work.files.begin(), work.files.end(), stem_less);
        // The staging capacity check keeps this true; the copy still refuses to overrun.
        if (work.files.size() <= files.size()) {
            std::copy(work.files.begin(), work.files.end(), files.begin());
            result.fileCount = work.files.size();
            result.cacheLeaves = std::move(work.cacheLeaves);
            result.complete = true;
            internal::save_remaps(work);
        }
    }
    return work.failed;
}

} // namespace

/** Checks each loaded mod's files in load order and fills the client overlay. */
Result build(const Paths& paths,
             std::span<const content_manifest::Row> installed,
             std::span<Record> records,
             std::span<const std::size_t> order,
             std::span<OverlayFile> files) noexcept {
    Result result{};
    try {
        const auto reserved = reserved_ids(paths);
        std::vector<Record> initial(records.begin(), records.end());
        // Each failed pass refuses one more loaded mod, so the loop ends.
        for (std::size_t pass = 0; pass <= order.size(); ++pass) {
            const std::size_t failed =
                run_pass(paths, installed, reserved, records, order, files, result);
            if (result.complete || failed >= records.size()
                || initial[failed].status != Status::loaded) {
                break;
            }
            const Problem* const refusal = problem::first_refusal(records[failed]);
            if (refusal == nullptr) {
                break;
            }
            // Run again from the starting records, with only the failed mod refused.
            problem::refuse(initial[failed],
                            refusal->reason,
                            "%.*s",
                            static_cast<int>(refusal->message.length),
                            refusal->message.bytes.data());
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=mods stage=packages mod=%s result=retry",
                              initial[failed].manifest.id.bytes.data());
            std::copy(initial.begin(), initial.end(), records.begin());
        }
    } catch (...) {
        result = {};
    }
    if (!result.complete) {
        for (const std::size_t record : order) {
            if (records[record].status == Status::loaded) {
                problem::refuse(
                    records[record], Reason::packageInvalid, "Mod files could not be checked.");
            }
        }
    }
    return result;
}

} // namespace sunrise::state::mods::packages
