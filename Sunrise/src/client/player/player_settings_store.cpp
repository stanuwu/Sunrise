/**
 * The player configuration store. Separate from Core settings because the interface changes these
 * values while the game runs and saves each one at once. Core settings are read once.
 */

#include "player_settings_store.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::player {
namespace {

/** The module-owned configuration file, beside the generated settings and logs. */
constexpr std::wstring_view kFileSuffix = L"\\player.json";
/** The document is a few scalars, so one small buffer covers both reading and writing. */
constexpr std::size_t kFileCapacity = 256;

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=player stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Finds one key and reads the boolean after it.
 * @param text Whole document.
 * @param key Quoted key to locate.
 * @param output Receives the value, untouched when the key is absent.
 */
void boolean_for(std::string_view text, std::string_view key, bool& output) noexcept {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) {
        return;
    }
    const std::size_t colon = text.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return;
    }
    std::size_t begin = colon + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    output = text.substr(begin).starts_with("true");
}

/**
 * Layers one document over the defaults. A missing or bad key keeps its default, so a hand-edited
 * file cannot stop the module loading.
 * @param text Whole document.
 * @param output Receives the parsed configuration.
 */
void parse(std::string_view text, Settings& output) noexcept {
    boolean_for(text, "\"infinite_ammo_enabled\"", output.infiniteAmmoEnabled);
    boolean_for(text, "\"no_turnback_enabled\"", output.noTurnbackEnabled);
    boolean_for(text, "\"godmode_enabled\"", output.godmodeEnabled);
}

/**
 * Writes the whole document. It is small enough that a complete rewrite is the simplest
 * correct save, which the shared settings file is not.
 * @param settings Configuration to store.
 * @return True when every byte reached the file.
 */
[[nodiscard]] bool store(const Settings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n"
                                   "  \"infinite_ammo_enabled\": %s,\n"
                                   "  \"no_turnback_enabled\": %s,\n"
                                   "  \"godmode_enabled\": %s\n"
                                   "}\n",
                                   settings.infiniteAmmoEnabled ? "true" : "false",
                                   settings.noTurnbackEnabled ? "true" : "false",
                                   settings.godmodeEnabled ? "true" : "false");
    if (size <= 0) {
        return false;
    }
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(size), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(size);
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

/** Reads the configuration file into the active settings when one exists. */
void load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0) {
        return;
    }
    parse(std::string_view(buffer.data(), read), g_settings);
}

} // namespace

/** Resolves the configuration file and loads it when one exists. */
void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops the runtime configuration and the resolved file path. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_path = core::path::Buffer{};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return One lock-consistent copy of the current configuration. */
Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Settings snapshot = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

/** Publishes one configuration and writes it straight to disk. */
bool publish(const Settings& settings) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = settings;
    const bool stored = store(settings);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

} // namespace sunrise::client::player
