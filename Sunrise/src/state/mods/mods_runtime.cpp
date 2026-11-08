#include "mods_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../core/ui/busy/busy.h"
#include "../../core/ui/notice/ui_notice_overlay.h"
#include "../../middleware/content/packages/reader/overlay.h"
#include "../content_manifest/content_manifest_state_runtime.h"
#include "../content_manifest/fingerprint/content_manifest_fingerprint.h"
#include "../content_manifest/scanner/package/content_manifest_package_name.h"
#include "config/mod_config.h"
#include "discovery/mod_discovery.h"
#include "manifest/mod_version.h"
#include "packages/mod_packages.h"
#include "problem/mod_problem.h"
#include "resolution/mod_resolution.h"

namespace sunrise::state::mods {
namespace {

namespace package_reader = middleware::content::packages::reader;

/** Merged package generations live below the Sunrise cache folder. */
constexpr std::wstring_view kMergeFolder = L"cache\\mods";
/** Package moves of remapped mods, kept beside the mods folder. */
constexpr std::wstring_view kRemapsFile = L"mod_remaps.json";
/** Separates a directory from its leaf. */
constexpr std::wstring_view kSeparator = L"\\";
/** One notice line stays free for the summary. */
constexpr std::size_t kSummaryLines = 1;
/** Raised when the mod packages cannot be served. */
constexpr std::string_view kManifestFailureNotice =
    "Mod packages could not be added to the game manifest.";
/** Hash label. The zero ends the name; the byte after it is the version. */
constexpr std::string_view kOverlayDomain{"SunriseModOverlay\0\1", 19};
/** Log placeholder for a mod whose manifest did not parse. */
constexpr char kUnknownId[] = "-";

/** The published mod list and the files the client opens for it. */
struct Storage final {
    SRWLOCK lock{SRWLOCK_INIT};
    /** Loaded mods first, in load order, then refused mods by folder. */
    std::array<Record, kModCapacity> records{};
    std::size_t count{};
    std::size_t loadedCount{};
    std::size_t mergedPackages{};
    /** Sorted by stem, so the client path lookup can search. */
    std::array<OverlayFile, kOverlayCapacity> files{};
    std::size_t fileCount{};
    std::array<wchar_t, kDirectoryCapacity> modsDirectory{};
    std::size_t modsDirectoryLength{};
    bool ready{};
};

Storage g_storage;
/** Boot works on these before publishing; one thread runs the boot. */
std::array<Record, kModCapacity> g_records{};
resolution::Order g_order{};
std::array<OverlayFile, kOverlayCapacity> g_stagedFiles{};

/**
 * What a package reload needs from boot. The manifest holds the mod rows once boot applied them,
 * so the installed rows are kept as boot saw them.
 */
struct ReloadInputs final {
    SRWLOCK lock{SRWLOCK_INIT};
    std::wstring gameDirectory;
    std::wstring packagesDirectory;
    std::wstring modsDirectory;
    std::wstring mergeDirectory;
    std::wstring remapsFile;
    std::vector<content_manifest::Row> installedRows;
    bool ready{};
};

ReloadInputs g_reload;

/** Carries the package stage through the installed-manifest visit. */
struct StageContext final {
    const packages::Paths* paths{};
    std::span<Record> records;
    std::span<const std::size_t> order;
    packages::Result result{};
};

/** Runs the package stage on the manifest rows the installed packages leave free. */
[[nodiscard]] packages::Result stage_packages(const StageContext& stage,
                                              std::span<const content_manifest::Row> installed,
                                              std::span<OverlayFile> output) noexcept {
    const std::size_t free = content_manifest::kRowCapacity - installed.size();
    return packages::build(*stage.paths,
                           installed,
                           stage.records,
                           stage.order,
                           output.first((std::min)(free, output.size())));
}

/** Clears published fields, leaving the initialized Windows lock alone. */
void clear_storage() noexcept {
    // Fill, not assignment from `{}`: that builds a whole-table temporary on the stack.
    g_storage.records.fill(Record{});
    g_storage.count = 0;
    g_storage.loadedCount = 0;
    g_storage.mergedPackages = 0;
    g_storage.files.fill(OverlayFile{});
    g_storage.fileCount = 0;
    g_storage.modsDirectory = {};
    g_storage.modsDirectoryLength = 0;
    g_storage.ready = false;
}

/**
 * Runs the package stage against the installed rows while the manifest stays locked.
 * The overlay may use only the manifest rows the installed packages leave free.
 */
[[nodiscard]] bool run_package_stage(void* opaque, const content_manifest::View& view) noexcept {
    auto& stage = *static_cast<StageContext*>(opaque);
    try {
        g_reload.installedRows.assign(view.rows.begin(), view.rows.end());
    } catch (...) {
        g_reload.installedRows.clear();
    }
    stage.result = stage_packages(stage, view.rows, g_stagedFiles);
    return true;
}

/**
 * Builds the absolute path of one served file: `gameDirectory\clientPath`.
 * @param gameDirectory Game folder, without a trailing separator.
 * @param file Served file.
 * @param output Receives the null-terminated path.
 * @return False when the path and its null do not fit.
 */
[[nodiscard]] bool served_path(std::wstring_view gameDirectory,
                               const OverlayFile& file,
                               package_reader::Path& output) noexcept {
    const std::size_t length = gameDirectory.size() + kSeparator.size() + file.clientPathLength;
    if (length + 1 > output.chars.size()) {
        return false;
    }
    auto cursor = std::copy(gameDirectory.begin(), gameDirectory.end(), output.chars.begin());
    cursor = std::copy(kSeparator.begin(), kSeparator.end(), cursor);
    // Client paths are ASCII, so each char widens unchanged.
    cursor = std::copy_n(file.clientPath.begin(), file.clientPathLength, cursor);
    *cursor = L'\0';
    return true;
}

/**
 * Publishes the served files to the package reader, so the server reads mod packages too.
 * @param gameDirectory Game folder, without a trailing separator.
 * @param files Staged served files.
 * @return False when a stem does not parse, a path does not fit, or the publish fails.
 */
[[nodiscard]] bool publish_reader_overlay(std::wstring_view gameDirectory,
                                          std::span<const OverlayFile> files) noexcept {
    std::vector<package_reader::overlay::File> served;
    try {
        served.resize(files.size());
    } catch (...) {
        return false;
    }
    for (std::size_t index = 0; index < files.size(); ++index) {
        const OverlayFile& file = files[index];
        content_manifest::scanner::package::ParsedName parsed{};
        if (!content_manifest::scanner::package::parse_stem(
                {file.row.name.data(), file.row.nameLength}, parsed)
            || !served_path(gameDirectory, file, served[index].path)) {
            return false;
        }
        served[index].packageId = file.row.packageId;
        served[index].patchIndex = parsed.patchIndex;
    }
    return package_reader::overlay::publish(served);
}

/**
 * Hashes every overlay row with its client path and the size and write time of its file.
 * @return False when a file is missing or the hash fails.
 */
[[nodiscard]] bool overlay_fingerprint(std::wstring_view gameDirectory,
                                       std::span<const OverlayFile> files,
                                       content_manifest::Fingerprint& output) noexcept {
    content_manifest::fingerprint::Hasher hasher;
    if (!hasher.update(std::as_bytes(std::span(kOverlayDomain.data(), kOverlayDomain.size())))) {
        return false;
    }
    package_reader::Path path{};
    for (const OverlayFile& file : files) {
        if (!served_path(gameDirectory, file, path)) {
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(
                core::path::extended(path.chars.data()).c_str(), GetFileExInfoStandard, &data)
            == FALSE) {
            return false;
        }
        ULARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        ULARGE_INTEGER time{};
        time.LowPart = data.ftLastWriteTime.dwLowDateTime;
        time.HighPart = data.ftLastWriteTime.dwHighDateTime;
        const auto name = std::as_bytes(std::span(file.row.name.data(), file.row.nameLength));
        const auto clientPath =
            std::as_bytes(std::span(file.clientPath.data(), file.clientPathLength));
        if (!content_manifest::fingerprint::update_u16(hasher, file.row.nameLength)
            || !hasher.update(name)
            || !content_manifest::fingerprint::update_u16(hasher, file.row.packageId)
            || !content_manifest::fingerprint::update_u64(hasher, file.row.buildSignature)
            || !content_manifest::fingerprint::update_u32(
                hasher, static_cast<std::uint32_t>(file.clientPathLength))
            || !hasher.update(clientPath)
            || !content_manifest::fingerprint::update_u64(hasher, size.QuadPart)
            || !content_manifest::fingerprint::update_u64(hasher, time.QuadPart)) {
            return false;
        }
    }
    return hasher.finish(output);
}

/**
 * Publishes the records, loaded first in load order, and the overlay files.
 * @param found Records the boot read.
 * @param orderCount Mods resolution placed.
 * @param stage Package stage result.
 * @param modsDirectory Absolute mods folder.
 */
void publish(std::size_t found,
             std::size_t orderCount,
             const packages::Result& stage,
             std::wstring_view modsDirectory) noexcept {
    clear_storage();
    for (std::size_t index = 0; index < orderCount; ++index) {
        const Record& record = g_records[g_order[index]];
        if (record.status == Status::loaded) {
            g_storage.records[g_storage.count++] = record;
        }
    }
    g_storage.loadedCount = g_storage.count;
    for (std::size_t index = 0; index < found; ++index) {
        if (g_records[index].status != Status::loaded) {
            g_storage.records[g_storage.count++] = g_records[index];
        }
    }
    g_storage.fileCount = stage.fileCount;
    std::copy_n(g_stagedFiles.begin(), stage.fileCount, g_storage.files.begin());
    g_storage.mergedPackages = stage.mergedPackages;
    g_storage.modsDirectoryLength = (std::min)(modsDirectory.size(), kDirectoryCapacity - 1);
    std::copy_n(
        modsDirectory.begin(), g_storage.modsDirectoryLength, g_storage.modsDirectory.begin());
    g_storage.ready = true;
}

/** @return Loaded mods that set `cheat`. The caller holds the lock. */
[[nodiscard]] std::size_t loaded_cheats() noexcept {
    const auto loaded = std::span(g_storage.records).first(g_storage.loadedCount);
    return static_cast<std::size_t>(std::count_if(
        loaded.begin(), loaded.end(), [](const Record& record) { return record.manifest.cheat; }));
}

/** Writes one line per mod: loaded with its order, or refused with each reason. */
void log_records() noexcept {
    for (std::size_t index = 0; index < g_storage.count; ++index) {
        const Record& record = g_storage.records[index];
        const char* id = record.manifestRead ? record.manifest.id.bytes.data() : kUnknownId;
        if (record.status == Status::loaded) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::info,
                              "ev=mods stage=load folder=%s mod=%s version=%s order=%zu "
                              "packages=%u patches=%u sdk=%d scripts=%d cheat=%d config=%zu "
                              "result=ok",
                              record.folder.bytes.data(),
                              id,
                              version::text(record.manifest.version).data(),
                              index,
                              record.newPackageFiles,
                              record.patchFiles,
                              record.hasSdk ? 1 : 0,
                              record.hasScripts ? 1 : 0,
                              record.manifest.cheat ? 1 : 0,
                              record.manifest.configCount);
        }
        for (std::size_t entry = 0; entry < record.problemCount; ++entry) {
            const Problem& problem = record.problems[entry];
            const bool refusal = problem.severity == Severity::refusal;
            core::log::writef(core::log::Channel::state,
                              refusal ? core::log::Level::warn : core::log::Level::info,
                              "ev=mods stage=load folder=%s mod=%s result=%s reason=%s",
                              record.folder.bytes.data(),
                              id,
                              refusal ? "refused" : "warn",
                              problem::reason_name(problem.reason));
        }
    }
}

/** Raises one notice per refused mod and one line for the rest. */
void raise_notices(std::size_t skipped) noexcept {
    // Core raises the cheat line after this; keep a slot free for it.
    const bool cheat = loaded_cheats() != 0 || core::settings::get().client.cheats;
    const std::size_t refusalLimit =
        core::ui::notice::kNoticeCapacity - kSummaryLines - (cheat ? 1U : 0U);
    std::size_t raised = 0;
    std::size_t unshown = 0;
    std::size_t warned = 0;
    for (std::size_t index = 0; index < g_storage.count; ++index) {
        const Record& record = g_storage.records[index];
        const Problem* const refusal = problem::first_refusal(record);
        if (refusal == nullptr) {
            warned += record.problemCount != 0 ? 1U : 0U;
            continue;
        }
        if (raised == refusalLimit) {
            ++unshown;
            continue;
        }
        std::array<char, core::ui::notice::kTextCapacity + 1> line{};
        const char* name =
            record.manifestRead ? record.manifest.name.bytes.data() : record.folder.bytes.data();
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "Mod %.*s did not load: %s",
                          static_cast<int>(record.manifestRead ? record.manifest.name.length
                                                               : record.folder.length),
                          name,
                          refusal->message.bytes.data());
        if (written > 0) {
            core::ui::notice::raise(
                {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
            ++raised;
        }
    }
    std::array<char, core::ui::notice::kTextCapacity + 1> summary{};
    int written = 0;
    if (unshown != 0 || skipped != 0) {
        written = std::snprintf(summary.data(),
                                summary.size(),
                                "%zu more mods did not load. See Sunrise > Mods.",
                                unshown + skipped);
    } else if (warned != 0) {
        written = std::snprintf(summary.data(),
                                summary.size(),
                                "%zu loaded mods have warnings. See Sunrise > Mods.",
                                warned);
    }
    if (written > 0) {
        core::ui::notice::raise(
            {summary.data(), (std::min)(static_cast<std::size_t>(written), summary.size() - 1)});
    }
}

/** @return `directory\leaf`, or empty when it does not fit. */
[[nodiscard]] std::wstring joined(std::wstring_view directory, std::wstring_view leaf) noexcept {
    try {
        std::wstring output(directory);
        output.append(kSeparator).append(leaf);
        return output;
    } catch (...) {
        return {};
    }
}

} // namespace

/** Finds, resolves and checks every installed mod, then publishes the list. */
bool initialize(const Directories& directories) noexcept {
    const std::wstring modsDirectory = joined(directories.artifactDirectory, kModsFolder);
    const std::wstring mergeDirectory = joined(directories.artifactDirectory, kMergeFolder);
    if (modsDirectory.empty() || mergeDirectory.empty()
        || modsDirectory.size() >= kDirectoryCapacity) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=initialize result=fail reason=path");
        return false;
    }
    // Create the folder so users can find where mods go.
    (void)CreateDirectoryW(modsDirectory.c_str(), nullptr);

    g_records.fill(Record{});
    g_order = {};
    const discovery::Result found = discovery::discover(modsDirectory, g_records);
    if (!found.complete) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=discovery result=fail");
        core::ui::notice::raise("Mod folders could not be listed. No mods are loaded.");
        return false;
    }
    const std::span<Record> records = std::span(g_records).first(found.count);
    const std::size_t orderCount = resolution::resolve(records, g_order);

    // The package bake reads the saved values.
    if (!config::load(directories.artifactDirectory)) {
        return false;
    }

    const std::wstring remapsFile = joined(directories.artifactDirectory, kRemapsFile);
    const packages::Paths paths{directories.gameDirectory,
                                directories.packagesDirectory,
                                modsDirectory,
                                mergeDirectory,
                                remapsFile};
    StageContext stage{&paths, records, std::span(g_order).first(orderCount)};
    // The stage also runs with no mods, so merged files from an earlier boot are removed.
    if (orderCount != 0) {
        core::ui::busy::begin(core::ui::busy::Task::manifestBuild);
    }
    const bool visited = content_manifest::visit_snapshot(&run_package_stage, &stage);
    if (orderCount != 0) {
        core::ui::busy::end(core::ui::busy::Task::manifestBuild);
    }
    if (!visited || !stage.result.complete) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=packages result=fail");
        core::ui::notice::raise("Mod packages could not be checked. No mods are loaded.");
        return false;
    }
    content_manifest::Fingerprint fingerprint{};
    const auto files = std::span(g_stagedFiles).first(stage.result.fileCount);
    if (!files.empty() && !overlay_fingerprint(directories.gameDirectory, files, fingerprint)) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=fingerprint result=fail");
        return false;
    }

    std::vector<content_manifest::Row> rows;
    try {
        rows.reserve(files.size());
        for (const OverlayFile& file : files) {
            rows.push_back(file.row);
        }
    } catch (...) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=manifest result=fail reason=memory");
        core::ui::notice::raise("Mods could not be loaded.");
        return false;
    }
    // The reader table goes first, so the manifest never lists a file the server cannot read.
    if (!files.empty()) {
        if (!publish_reader_overlay(directories.gameDirectory, files)) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::error,
                              "ev=mods stage=reader_overlay result=fail files=%zu",
                              files.size());
            core::ui::notice::raise(kManifestFailureNotice);
            return false;
        }
        core::log::writef(core::log::Channel::state,
                          core::log::Level::info,
                          "ev=mods stage=reader_overlay result=ok files=%zu",
                          files.size());
    }
    if (!content_manifest::apply_overlay(rows, fingerprint)) {
        package_reader::overlay::clear();
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=mods stage=manifest result=fail");
        core::ui::notice::raise(kManifestFailureNotice);
        return false;
    }

    AcquireSRWLockExclusive(&g_storage.lock);
    publish(found.count, orderCount, stage.result, modsDirectory);
    config::publish(records);
    log_records();
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=mods stage=initialize folders=%zu loaded=%zu refused=%zu skipped=%zu "
                      "files=%zu merged=%zu result=ok",
                      found.count,
                      g_storage.loadedCount,
                      g_storage.count - g_storage.loadedCount,
                      found.skipped,
                      g_storage.fileCount,
                      g_storage.mergedPackages);
    raise_notices(found.skipped);
    ReleaseSRWLockExclusive(&g_storage.lock);
    AcquireSRWLockExclusive(&g_reload.lock);
    try {
        g_reload.gameDirectory = directories.gameDirectory;
        g_reload.packagesDirectory = directories.packagesDirectory;
        g_reload.modsDirectory = modsDirectory;
        g_reload.mergeDirectory = mergeDirectory;
        g_reload.remapsFile = remapsFile;
        g_reload.ready = true;
    } catch (...) {
        g_reload.ready = false;
    }
    ReleaseSRWLockExclusive(&g_reload.lock);
    packages::prune(paths, stage.result);
    g_records.fill(Record{});
    g_stagedFiles.fill(OverlayFile{});
    return true;
}

namespace {

/** Writes the reason into the result and the log. */
ReloadResult reload_failure(const char* reason, std::string_view detail = {}) noexcept {
    ReloadResult result{};
    std::snprintf(result.message.data(),
                  result.message.size(),
                  "%s%s%.*s",
                  reason,
                  detail.empty() ? "" : " ",
                  static_cast<int>(detail.size()),
                  detail.data());
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=mods stage=reload result=refused reason=\"%s\"",
                      result.message.data());
    return result;
}

/** @return The ids of the mods that loaded at boot, in load order. */
[[nodiscard]] std::vector<std::string> boot_loaded_ids() {
    std::vector<std::string> ids;
    AcquireSRWLockShared(&g_storage.lock);
    ids.reserve(g_storage.loadedCount);
    for (std::size_t index = 0; index < g_storage.loadedCount; ++index) {
        ids.emplace_back(g_storage.records[index].manifest.id.view());
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return ids;
}

/** @return True when the two tables serve the same generations from the same files. */
[[nodiscard]] bool same_table(std::span<const package_reader::overlay::File> left,
                              std::span<const package_reader::overlay::File> right) noexcept {
    return std::equal(
        left.begin(), left.end(), right.begin(), right.end(), [](const auto& a, const auto& b) {
            return a.packageId == b.packageId && a.patchIndex == b.patchIndex
                   && std::wstring_view(a.path.chars.data())
                          == std::wstring_view(b.path.chars.data());
        });
}

} // namespace

/**
 * Stages every mod again exactly as boot did and publishes the server's reader table. The
 * client's manifest is not touched, and cache files are not pruned, so nothing the game opened
 * goes away.
 */
ReloadResult reload_packages() noexcept {
    if (TryAcquireSRWLockExclusive(&g_reload.lock) == FALSE) {
        return reload_failure("A package reload is already running.");
    }
    struct Unlock final {
        ~Unlock() {
            ReleaseSRWLockExclusive(&g_reload.lock);
        }
    } unlock;
    if (!g_reload.ready || g_reload.installedRows.empty()) {
        return reload_failure(
            "Mods did not finish loading at boot, so their packages cannot be reloaded.");
    }
    try {
        auto records = std::make_unique<std::array<Record, kModCapacity>>();
        auto order = std::make_unique<resolution::Order>();
        auto files = std::make_unique<std::array<OverlayFile, kOverlayCapacity>>();
        const discovery::Result found = discovery::discover(g_reload.modsDirectory, *records);
        if (!found.complete) {
            return reload_failure("Mod folders could not be listed.");
        }
        const std::span<Record> listed = std::span(*records).first(found.count);
        const std::size_t orderCount = resolution::resolve(listed, *order);
        const packages::Paths paths{g_reload.gameDirectory,
                                    g_reload.packagesDirectory,
                                    g_reload.modsDirectory,
                                    g_reload.mergeDirectory,
                                    g_reload.remapsFile};
        const StageContext stage{&paths, listed, std::span(*order).first(orderCount)};
        const packages::Result staged = stage_packages(stage, g_reload.installedRows, *files);
        if (!staged.complete) {
            return reload_failure("Mod files could not be checked.");
        }
        // The same mods must load, or the reader would serve a set the client never saw.
        const std::vector<std::string> before = boot_loaded_ids();
        for (const std::string& id : before) {
            const auto record =
                std::find_if(listed.begin(), listed.end(), [&id](const Record& value) {
                    return value.manifest.id.view() == id;
                });
            if (record == listed.end() || record->status != Status::loaded) {
                const Problem* refusal =
                    record == listed.end() ? nullptr : problem::first_refusal(*record);
                return reload_failure("Mod",
                                      refusal != nullptr ? std::string(id) + " no longer loads: "
                                                               + refusal->message.bytes.data()
                                                         : std::string(id) + " is gone.");
            }
        }
        const std::size_t loaded = static_cast<std::size_t>(
            std::count_if(listed.begin(), listed.end(), [](const Record& value) {
                return value.status == Status::loaded;
            }));
        if (loaded != before.size()) {
            return reload_failure(
                "A mod that did not load at boot loads now. Restart the game to add it.");
        }
        const auto served = std::span(*files).first(staged.fileCount);
        std::vector<package_reader::overlay::File> table;
        table.resize(served.size());
        for (std::size_t index = 0; index < served.size(); ++index) {
            content_manifest::scanner::package::ParsedName parsed{};
            if (!content_manifest::scanner::package::parse_stem(
                    {served[index].row.name.data(), served[index].row.nameLength}, parsed)
                || !served_path(g_reload.gameDirectory, served[index], table[index].path)) {
                return reload_failure("A served package path does not fit.");
            }
            table[index].packageId = served[index].row.packageId;
            table[index].patchIndex = parsed.patchIndex;
        }
        std::sort(table.begin(), table.end(), [](const auto& first, const auto& second) {
            return first.packageId != second.packageId ? first.packageId < second.packageId
                                                       : first.patchIndex < second.patchIndex;
        });
        ReloadResult result{.ok = true, .files = table.size()};
        // An unchanged table is not published, so readers keep their caches.
        result.changed = !same_table(table, package_reader::overlay::files());
        if (result.changed && !package_reader::overlay::publish(table)) {
            return reload_failure("The new package table could not be published.");
        }
        core::log::writef(core::log::Channel::state,
                          core::log::Level::info,
                          "ev=mods stage=reload result=ok files=%zu changed=%d",
                          table.size(),
                          result.changed ? 1 : 0);
        return result;
    } catch (...) {
        return reload_failure("The package reload ran out of memory.");
    }
}

/** Clears the published mod list and the config values. */
void shutdown() noexcept {
    config::shutdown();
    AcquireSRWLockExclusive(&g_storage.lock);
    clear_storage();
    ReleaseSRWLockExclusive(&g_storage.lock);
}

/** Visits the published mod list while State holds its shared lock. */
bool visit(Visitor visitor, void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_storage.lock);
    if (!g_storage.ready) {
        ReleaseSRWLockShared(&g_storage.lock);
        return false;
    }
    const View view{
        g_storage.records.data(),
        g_storage.count,
        g_storage.loadedCount,
        g_storage.mergedPackages,
        {g_storage.modsDirectory.data(), g_storage.modsDirectoryLength},
    };
    const bool result = visitor(context, view);
    ReleaseSRWLockShared(&g_storage.lock);
    return result;
}

/** @return The number of package files loaded mods serve to the client. */
std::size_t file_count() noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const std::size_t count = g_storage.ready ? g_storage.fileCount : 0;
    ReleaseSRWLockShared(&g_storage.lock);
    return count;
}

/** @return Loaded mods that set `cheat`. */
std::size_t cheat_count() noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const std::size_t count = g_storage.ready ? loaded_cheats() : 0;
    ReleaseSRWLockShared(&g_storage.lock);
    return count;
}

/** Finds where the client must open one mod package file. */
bool find_client_path(std::string_view stem, std::span<char> output) noexcept {
    AcquireSRWLockShared(&g_storage.lock);
    const auto files = std::span(g_storage.files).first(g_storage.ready ? g_storage.fileCount : 0);
    const auto found = std::lower_bound(
        files.begin(), files.end(), stem, [](const OverlayFile& file, std::string_view key) {
            return std::string_view(file.row.name.data(), file.row.nameLength) < key;
        });
    const bool matched = found != files.end()
                         && std::string_view(found->row.name.data(), found->row.nameLength) == stem
                         && found->clientPathLength < output.size();
    if (matched) {
        std::copy_n(found->clientPath.begin(), found->clientPathLength, output.begin());
        output[found->clientPathLength] = '\0';
    }
    ReleaseSRWLockShared(&g_storage.lock);
    return matched;
}

} // namespace sunrise::state::mods
