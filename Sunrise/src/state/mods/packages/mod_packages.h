#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../definition.h"

namespace sunrise::state::mods::packages {

/** Directories the package stage reads and writes. Each has no trailing separator. */
struct Paths final {
    /** Holds `packages`; client paths are relative to it. */
    std::wstring_view gameDirectory;
    std::wstring_view packagesDirectory;
    std::wstring_view modsDirectory;
    /** Receives merged generations. Stale files in it are removed. */
    std::wstring_view mergeDirectory;
    /** Package moves kept across sessions, so a remapped mod keeps its ids. */
    std::wstring_view remapsFile;
};

/** What the package stage produced. */
struct Result final {
    std::size_t fileCount{};
    std::size_t mergedPackages{};
    /** False when the stage could not run; every loaded mod is then refused. */
    bool complete{};
    /** Cache files retained only after this result has been published. */
    std::vector<std::wstring> cacheLeaves;
};

/**
 * Checks each loaded mod's packages and patches in load order, merges patches that stack on
 * one package, and fills the client overlay. A mod whose files fail is refused, and so is
 * every mod that depends on it.
 * @param paths Directories.
 * @param installed Installed manifest rows.
 * @param records Every mod record.
 * @param order Load order from resolution.
 * @param files Receives the overlay files, sorted by stem.
 * @return Counts and completion.
 */
[[nodiscard]] Result build(const Paths& paths,
                           std::span<const content_manifest::Row> installed,
                           std::span<Record> records,
                           std::span<const std::size_t> order,
                           std::span<OverlayFile> files) noexcept;

/** Removes stale cache files only after mod, config and overlay publication all succeed. */
void prune(const Paths& paths, const Result& published) noexcept;

} // namespace sunrise::state::mods::packages
