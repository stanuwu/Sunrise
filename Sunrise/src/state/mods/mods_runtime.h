#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::state::mods {

/** The mods folder inside the Sunrise folder. */
inline constexpr std::wstring_view kModsFolder = L"mods";

/** Directories the loader works in. None has a trailing separator. */
struct Directories final {
    /** Holds the game executable and `packages`; client paths are relative to it. */
    std::wstring_view gameDirectory;
    std::wstring_view packagesDirectory;
    /** The Sunrise folder: mods live in `mods`, merged packages in `cache\mods`. */
    std::wstring_view artifactDirectory;
};

/**
 * Finds, resolves and checks every installed mod, publishes the list with its package rows, and
 * loads the saved config values. Runs after the installed manifest is published and before the
 * client registers packages, because the manifest request serves the mod packages.
 * @param directories Game, packages and Sunrise folders.
 * @return False only when the mod list could not be published at all.
 */
[[nodiscard]] bool initialize(const Directories& directories) noexcept;

/** Clears the published mod list and the config values. */
void shutdown() noexcept;

/** Reload refusal text with its null; it must fit one Studio command reply. */
inline constexpr std::size_t kReloadMessageCapacity = 192;

/** What a package reload did. */
struct ReloadResult final {
    bool ok{};
    /** True when the reader now serves a different table. */
    bool changed{};
    std::size_t files{};
    /** Why the reload was refused; empty when ok. */
    std::array<char, kReloadMessageCapacity> message{};
};

/**
 * Checks every mod's package files again, exactly as boot does, and points the server's package
 * reader at the result. The previous table stays live when any mod would no longer load.
 * Runs on one thread at a time; a second call while one runs is refused.
 */
[[nodiscard]] ReloadResult reload_packages() noexcept;

/**
 * Visits the published mod list while State holds its shared lock.
 * @param visitor Non-reentrant callback.
 * @param context Caller context passed to the callback.
 * @return False when nothing is published or the callback refused the view.
 */
[[nodiscard]] bool visit(Visitor visitor, void* context) noexcept;

/** @return The number of package files loaded mods serve to the client. */
[[nodiscard]] std::size_t file_count() noexcept;

/** @return Loaded mods that set `cheat`. A refused mod does not count. */
[[nodiscard]] std::size_t cheat_count() noexcept;

/**
 * Finds where the client must open one mod package file.
 * @param stem Package stem the client asked for, without extension.
 * @param output Receives the null-terminated path relative to the game directory.
 * @return True when a loaded mod serves this stem and the path fits.
 */
[[nodiscard]] bool find_client_path(std::string_view stem, std::span<char> output) noexcept;

} // namespace sunrise::state::mods
