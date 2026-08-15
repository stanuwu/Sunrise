/**
 * The teleport configuration store. It is separate from Core settings because the interface
 * changes these values while the game runs and saves each change at once. Core settings are
 * parsed once into an immutable global.
 */

#include "teleport_settings_store.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::teleport {
namespace {

/** The module-owned configuration file, beside the generated settings and logs. */
constexpr std::wstring_view kFileSuffix = L"\\teleport.json";
/** The document is 3 scalars, so one small buffer covers both reading and writing. */
constexpr std::size_t kFileCapacity = 512;
/** Longest scalar accepted from the file. Anything longer is malformed rather than large. */
constexpr std::size_t kScalarCapacity = 32;
/** Highest Windows virtual-key code, so a stored binding cannot name a key that cannot exist. */
constexpr std::uint32_t kMaximumVirtualKey = 254;

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

/** @param settings Candidate configuration. @return True when every field is in range. */
[[nodiscard]] bool valid(const Settings& settings) noexcept {
    return settings.distance >= kMinimumDistance && settings.distance <= kMaximumDistance
           && settings.virtualKey <= kMaximumVirtualKey;
}

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=teleport stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Finds one key's raw scalar text.
 * @param text Whole document.
 * @param key Quoted key to locate.
 * @param output Receives the text between the colon and the next separator.
 * @return True when the key exists and carries a non-empty value.
 */
[[nodiscard]] bool
scalar_for(std::string_view text, std::string_view key, std::string_view& output) noexcept {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t begin = colon + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = begin;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n'
           && text[end] != '\r') {
        ++end;
    }
    output = text.substr(begin, end - begin);
    return !output.empty();
}

/**
 * Copies one scalar into null-terminated storage the C conversions require.
 * @param value Scalar text taken from the document.
 * @param output Receives the terminated copy.
 * @return True when the scalar fits.
 */
[[nodiscard]] bool terminated(std::string_view value,
                              std::array<char, kScalarCapacity>& output) noexcept {
    if (value.size() >= output.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = value[index];
    }
    output[value.size()] = '\0';
    return true;
}

/**
 * Layers one document over the current defaults. A missing or malformed key keeps its default,
 * so a hand-edited file cannot stop the module loading.
 * @param text Whole document.
 * @param output Receives the parsed configuration.
 */
void parse(std::string_view text, Settings& output) noexcept {
    std::string_view scalar;
    if (scalar_for(text, "\"enabled\"", scalar)) {
        output.enabled = scalar.starts_with("true");
    }
    std::array<char, kScalarCapacity> buffer{};
    if (scalar_for(text, "\"distance\"", scalar) && terminated(scalar, buffer)) {
        output.distance = std::strtof(buffer.data(), nullptr);
    }
    if (scalar_for(text, "\"virtual_key\"", scalar) && terminated(scalar, buffer)) {
        output.virtualKey = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
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
                                   "{\n  \"enabled\": %s,\n  \"distance\": %.3f,\n"
                                   "  \"virtual_key\": %u\n}\n",
                                   settings.enabled ? "true" : "false",
                                   static_cast<double>(settings.distance),
                                   static_cast<unsigned>(settings.virtualKey));
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
    Settings parsed{};
    parse(std::string_view(buffer.data(), read), parsed);
    if (!valid(parsed)) {
        report_fail("range");
        return;
    }
    g_settings = parsed;
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
    if (!valid(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings = settings;
    const bool stored = store(settings);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

} // namespace sunrise::client::teleport
