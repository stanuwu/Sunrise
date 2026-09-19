#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

#include "../../../resources/resource.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "parser.h"
#include "settings.h"
#include "steam/platform_identity.h"

namespace sunrise::core::settings {
namespace {

/** The JSON settings file is the only file stored directly in the owned folder. */
constexpr std::wstring_view kSettingsFileSuffix = L"\\settings.json";
/** Largest settings file accepted into fixed storage. */
constexpr std::size_t kConfigCapacity = 1024 * 1024;

Settings g_settings = defaults();

/**
 * Names the step that ended the load. Settings are read before the log sinks exist, so this line
 * is the only way to report a boot failure here.
 * @param reason Short key naming the step.
 * @return Always false, so callers can return it directly.
 */
[[nodiscard]] bool fail(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings result=fail reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/**
 * Reports a settings version that differs from this build.
 * @param fileVersion Parsed version, defaulting to kSettingsVersion when the key was missing.
 */
void report_version(std::uint32_t fileVersion) noexcept {
    if (fileVersion == kSettingsVersion) {
        return;
    }
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=version result=mismatch file=%u build=%u",
                                      static_cast<unsigned>(fileVersion),
                                      static_cast<unsigned>(kSettingsVersion));
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Borrows the default settings document out of the module resources.
 * @param module Loaded DLL holding the default JSON resource.
 * @param output Receives the resource bytes, owned by the module.
 * @return True when the resource is present and not empty.
 */
[[nodiscard]] bool bundled_document(void* module, std::string_view& output) noexcept {
    const HMODULE loadedModule = static_cast<HMODULE>(module);
    const HRSRC resource =
        FindResourceW(loadedModule, MAKEINTRESOURCEW(IDR_DEFAULT_SETTINGS), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(loadedModule, resource);
    const HGLOBAL loaded = LoadResource(loadedModule, resource);
    const auto* bytes =
        loaded != nullptr ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    output = std::string_view(bytes, size);
    return true;
}

/**
 * Copies the bundled default settings. An existing file is never overwritten.
 * @param module Loaded DLL holding the default JSON resource.
 * @param configPath Null-terminated destination path.
 * @return True when every bundled byte is written and the file closes cleanly.
 */
[[nodiscard]] bool write_default(void* module, const path::Buffer& configPath) noexcept {
    std::string_view document;
    if (!bundled_document(module, document)) {
        return false;
    }
    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        // A half-written default must not become the next boot's settings.
        (void)DeleteFileW(configPath.chars.data());
    }
    return complete;
}

/**
 * Drops a leading UTF-8 byte order mark.
 * Common editors write one and the parser would read it as a stray token, which fails startup
 * before the log opens.
 * @param document Whole settings text as read from disk.
 * @return The same text with any BOM removed.
 */
[[nodiscard]] std::string_view without_byte_order_mark(std::string_view document) noexcept {
    // UTF-8 byte order mark. An editor writes it and the parser must not see it.
    constexpr std::string_view kMark = "\xEF\xBB\xBF";
    return document.starts_with(kMark) ? document.substr(kMark.size()) : document;
}

} // namespace

/** Loads the settings file from the owned folder, or creates the default one. */
bool initialize(void* module) noexcept {
    shutdown();
    path::Buffer configPath;
    if (!path::artifact_directory(module, configPath)
        || !path::append(configPath, kSettingsFileSuffix)) {
        return fail("path");
    }

    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    HANDLE readableFile = file;
    if (readableFile == INVALID_HANDLE_VALUE) {
        // A missing file is created once; other open failures remain fatal.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return fail("open");
        }
        if (!write_default(module, configPath)) {
            return fail("write_default");
        }
        readableFile = CreateFileW(configPath.chars.data(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
        if (readableFile == INVALID_HANDLE_VALUE) {
            return fail("reopen");
        }
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(readableFile, &size) || size.QuadPart <= 0) {
        CloseHandle(readableFile);
        return fail("empty");
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > kConfigCapacity) {
        // Silence here reads exactly like a crash, and the cap is the usual cause.
        CloseHandle(readableFile);
        return fail("too_large");
    }

    const std::unique_ptr<std::array<char, kConfigCapacity>> buffer{
        new (std::nothrow) std::array<char, kConfigCapacity>{}};
    if (!buffer) {
        CloseHandle(readableFile);
        return fail("allocate");
    }
    DWORD read = 0;
    const bool readOk =
        ReadFile(readableFile, buffer->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
            != FALSE
        && read == size.QuadPart;
    const bool closed = CloseHandle(readableFile) != FALSE;
    if (!readOk || !closed) {
        return fail("read");
    }
    std::string_view document = without_byte_order_mark(std::string_view(buffer->data(), read));
    std::uint32_t version = 0;
    bool compact = false;
    if (!parser::Parser(document).parse_version(version, &compact)) {
        return fail("version");
    }
    // The compact document has no schema history, so only a versioned or full file is replaced.
    if (version < kSettingsVersion && (version != 0 || !compact)) {
        if (!DeleteFileW(configPath.chars.data())) {
            return fail("delete_old");
        }
        if (!write_default(module, configPath) || !bundled_document(module, document)) {
            return fail("write_default");
        }
    }
    Settings parsed;
    ParseFailure parseFailure{};
    if (!parse(document, parsed, &parseFailure)) {
        return fail(parseFailure == ParseFailure::multiplayerOptInRequired
                        ? "multiplayer_opt_in_required"
                        : "parse");
    }
    report_version(parsed.version);
    Role selectedRole = parsed.hasConfiguredRole ? parsed.configuredRole : Role::embedded;
    if (parsed.compactClient || parsed.compactHost) {
        selectedRole = parsed.compactHost ? Role::host : Role::client;
        if (!parsed.steam.user.hasConfiguredSteamId
            && !steam::platform_identity::load_or_create(parsed.steam.user.steamId)) {
            return fail("identity_cache");
        }
        parsed.version = kSettingsVersion;
        parsed.configuredRole = selectedRole;
        parsed.client.externalServer.enabled = false;
        if (parsed.compactHost) {
            parsed.server.bapBind = {0, 0, 0, 0};
            parsed.server.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.gameplay.bindAddress = {0, 0, 0, 0};
            parsed.server.gameplay.advertisedAddress = parsed.client.serverEndpoint.address;
            parsed.server.gameplay.transportAddress = parsed.client.serverEndpoint.address;
        } else {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
            parsed.server.bapPort = 0;
        }
    }
    if (selectedRole == Role::client && !parsed.client.externalServer.enabled) {
        if (!parsed.server.upstream.enabled) {
            parsed.server.upstream.enabled = true;
            parsed.server.upstream.host = parsed.client.serverEndpoint.host;
            parsed.server.upstream.address = parsed.client.serverEndpoint.address;
            parsed.server.upstream.bapPort = parsed.client.serverEndpoint.bapPort;
        }
        parsed.server.bapPort = 0;
    }
    if (selectedRole == Role::host && parsed.server.upstream.enabled) {
        return fail("host_upstream");
    }
    if (selectedRole == Role::host) {
        if (parsed.client.externalServer.enabled) {
            return fail("host_external");
        }
        if (parsed.server.bapPort == 0
            || (parsed.server.bapBind != std::array<unsigned char, 4>{0, 0, 0, 0}
                && parsed.server.bapBind != std::array<unsigned char, 4>{127, 0, 0, 1})) {
            return fail("host_local_listener");
        }
        if (parsed.server.gameplay.topology != server::gameplay::Topology::embedded) {
            return fail("host_gameplay");
        }
    }
    for (std::size_t i = 0; i < parsed.server.upstream.host.size(); ++i) {
        parsed.server.upstream.hostWide[i] = static_cast<wchar_t>(parsed.server.upstream.host[i]);
    }
    // Publish the role only after every configuration and identity check succeeds.
    static_cast<void>(configure_role(selectedRole, true));
    g_settings = parsed;
    return true;
}

/** Resets active settings to the fixed defaults. */
void shutdown() noexcept {
    g_settings = defaults();
    (void)configure_role(Role::embedded, false);
}

/** @return Active read-only Core settings. */
const Settings& get() noexcept {
    return g_settings;
}

} // namespace sunrise::core::settings
