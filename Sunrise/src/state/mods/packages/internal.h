#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/merge/merge.h"
#include "../../../middleware/content/packages/merge/remap.h"
#include "../../../middleware/content/packages/reader/package_file.h"
#include "../../../middleware/content/packages/table_merge/sidecar.h"
#include "../../content_manifest/definition.h"
#include "../../content_manifest/fingerprint/content_manifest_fingerprint.h"
#include "../../content_manifest/scanner/package/content_manifest_package_name.h"
#include "../definition.h"
#include "mod_packages.h"

namespace sunrise::state::mods::packages::internal {

namespace merge = middleware::content::packages::merge;
namespace table_merge = middleware::content::packages::table_merge;

/** One table row per package id the registrar accepts. */
inline constexpr std::size_t kPackageIdCount = std::size_t{content_manifest::kMaximumPackageId} + 1;
/** Owner of a base generation that comes from the installed packages. */
inline constexpr std::size_t kInstalledOwner = static_cast<std::size_t>(-1);
/** Names no record. */
inline constexpr std::size_t kNoRecord = static_cast<std::size_t>(-1);
/** Names no parsed merge data. */
inline constexpr std::size_t kNoSidecar = static_cast<std::size_t>(-1);
/** Separates a directory from its leaf. */
inline constexpr std::wstring_view kSeparator = L"\\";
using middleware::content::packages::reader::layout::kPackageFileExtension;

/** The newest generation of one package id, installed or added by an earlier mod. */
struct Base final {
    content_manifest::Row newest{};
    /** Stem characters before `_<patch>`: the part every generation shares. */
    std::uint16_t identityLength{};
    std::uint32_t patch{};
    std::size_t owner{kInstalledOwner};
    bool present{};
};

/** One mod patch waiting to be served directly or merged. */
struct Claim final {
    std::size_t record{};
    std::wstring path;
    OverlayFile file{};
    std::bitset<merge::kEntryLimit> changed{};
    /** Index into Work::sidecars, or kNoSidecar. */
    std::size_t sidecar{kNoSidecar};
    /** Patch index of the claimed file. */
    std::uint32_t generation{};
    /** The mod ships consecutive patches; the older ones are served as parents. */
    bool chained{};
};

/** One staged new package file, kept for its merge data. */
struct AddedFile final {
    std::size_t record{};
    std::wstring path;
    std::uint16_t packageId{};
    std::uint32_t patch{};
    /** Index into Work::sidecars, or kNoSidecar. */
    std::size_t sidecar{kNoSidecar};
};

/** Entries of one package id that the table merge merged without a fallback. */
struct MergedEntries final {
    std::uint16_t packageId{};
    std::bitset<merge::kEntryLimit> entries{};
};

/** Hashes input files and keeps them open read-only until the cache file is checked or written. */
class SourceInputs final {
public:
    SourceInputs() = default;
    ~SourceInputs();
    SourceInputs(const SourceInputs&) = delete;
    SourceInputs& operator=(const SourceInputs&) = delete;
    /** Opens the file read-only, keeps it open, and adds its size, path and bytes to the key. */
    [[nodiscard]] bool hash(content_manifest::fingerprint::Hasher& hasher,
                            const std::wstring& path);

private:
    std::vector<void*> handles_;
};

/** How reading one whole file ended. */
enum class FileRead : std::uint8_t { ok, missing, tooLarge, failed };

/**
 * Reads one whole file in a single read.
 * @param limit Largest size accepted; it keeps the file inside one ReadFile call.
 * @param bytes Receives the file; its content is unusable unless the result is ok.
 * @return ok, missing when no such file exists, tooLarge past `limit`, or failed.
 */
[[nodiscard]] FileRead
read_whole_file(const std::wstring& path, std::size_t limit, std::vector<std::byte>& bytes);

/** One `.pkg` file listed in a mod subfolder. */
struct Listed final {
    std::wstring path;
    /** ASCII form of the leaf for messages; other characters become '?'. */
    std::string leaf;
    content_manifest::scanner::package::ParsedName parsed{};
    content_manifest::scanner::package::NameStatus status{};
};

/** One mod's package moves, kept across sessions while its manifest names the same packages. */
struct SavedRemap final {
    std::string modId;
    std::vector<std::uint16_t> packages;
    std::vector<merge::remap::Move> moves;
};

/** One loaded mod's exact typed recipe, applied once to the final served package. */
struct Bound final {
    std::size_t record{};
    ConfigBinding site{};
    ConfigValue value{};
};

/** Everything the package stage builds before it publishes. */
struct Work final {
    const Paths& paths;
    std::span<Record> records;
    /** Indexed by package id. */
    std::vector<Base> bases;
    /** In load order until the claims are served. */
    std::vector<Claim> claims;
    std::vector<OverlayFile> files;
    std::vector<OverlayFile> rawParents;
    std::vector<Bound> bindings;
    std::vector<std::wstring> cacheLeaves;
    /** Parsed merge data. Claims and added files name it by index. */
    std::vector<table_merge::Sidecar> sidecars;
    std::vector<AddedFile> added;
    /** Whole entries the rewrite serves, sorted by package id then entry. */
    std::vector<table_merge::Replacement> replacements;
    /** Records with an input to any replacement, sorted and unique. */
    std::vector<std::size_t> replacementMods;
    std::vector<MergedEntries> merged;
    /** Moves read from the remap file before the stage, and the moves this stage made. */
    std::vector<SavedRemap> savedRemaps;
    std::vector<SavedRemap> remaps;
    /** Package ids of the installed startup `_unp1_` packages; no mod may add them. */
    std::bitset<kPackageIdCount> reserved{};
    std::size_t fileCapacity{};
    /** The record whose refusal stopped the stage, or kNoRecord. */
    std::size_t failed{kNoRecord};
    bool complete{true};
};

/** @return `directory\leaf`. */
[[nodiscard]] std::wstring join(std::wstring_view directory, std::wstring_view leaf);

/** @return The ASCII stem of one row as a wide string. */
[[nodiscard]] std::wstring widen(std::string_view text);

/** @return The file of one package id's newest generation. */
[[nodiscard]] std::wstring base_path(const Work& work, const Base& base);

/** @return The absolute folder of one mod. */
[[nodiscard]] std::wstring mod_directory(const Work& work, const Record& record);

/** @return `<stem><infix><64 hex digits>.pkg`, naming one cache file by its key. */
[[nodiscard]] std::wstring cache_leaf_name(std::string_view stem,
                                           std::wstring_view infix,
                                           const content_manifest::Fingerprint& digest);

/** @return The package id of an entry tag. */
[[nodiscard]] std::uint16_t package_id(std::uint32_t tag) noexcept;

/** Warns a mod that the merge data of one of its package files was not used. */
void warn_ignored(Record& record, std::uint16_t packageId, const char* reason);

/**
 * Sets a file's client path: relative to the game directory and ASCII, as the client takes it.
 * @return False when the file is outside the game directory, not ASCII, or too long.
 */
[[nodiscard]] bool client_path(const Work& work, std::wstring_view absolute, OverlayFile& file);

/** Checks one loaded mod's packages and patches, and commits them if the mod stays loaded. */
void stage_mod(Work& work, std::size_t record);

/**
 * Moves a mod's own packages off ids another source holds, using its reference map: rewritten
 * copies replace the listed files. A mod with no usable map is left to the usual refusal.
 * @param added The mod's `packages` listing; @param patches its `patches` listing.
 * @return False when the mod was refused.
 */
[[nodiscard]] bool relocate(Work& work,
                            std::size_t record,
                            const std::wstring& directory,
                            std::vector<Listed>& added,
                            std::vector<Listed>& patches);

/** Reads the remap file; a missing or unreadable one reads as no saved moves. */
void load_remaps(Work& work);

/** Writes the remap file when its content changed: this stage's moves plus those of absent mods. */
void save_remaps(const Work& work);

/**
 * Merges the tables mods share, then serves each patched package id: one patch directly,
 * several through one merged generation.
 * @return The number of merged packages.
 */
[[nodiscard]] std::size_t serve_claims(Work& work);

/**
 * Drops every replacement, so each package serves later-wins, and warns each mod that had rows.
 * Merged rows and shifted sites span packages, so they are served all together or not at all.
 * @param packageId The package that could not be served merged.
 * @param reason Why, for the warning.
 */
void drop_replacements(Work& work, std::uint16_t packageId, const char* reason);

/** Warns each later mod about entries an earlier mod also changes that are not served merged. */
void warn_overlaps(Work& work);

/** Every binding must resolve to a plaintext entry supplied by its declaring mod. */
[[nodiscard]] bool admit_bindings(Work& work,
                                  std::size_t record,
                                  std::span<const OverlayFile> files,
                                  std::span<const Claim> claims,
                                  std::vector<Bound>& output);

/**
 * Writes merged entries and config values once into each final newest served package.
 * If any merged entry cannot be written, all are dropped and only config values are written.
 * @return False when a package with required config bindings could not be written.
 */
[[nodiscard]] bool rewrite_packages(Work& work);

/** Creates the cache directory without deleting any prior publication. */
[[nodiscard]] bool ensure_merge_directory(const Work& work);

} // namespace sunrise::state::mods::packages::internal
