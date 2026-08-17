#include "spawn_keybind_store.h"

#include <Windows.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::spawn {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\spawn_keybinds.json";
constexpr std::size_t kFileCapacity = 512;
constexpr std::uint32_t kMaximumVirtualKey = 254;
constexpr std::array<std::string_view, kActionCount> kNames{
    "main_player",
    "main_crosshair",
    "projectile_player",
    "projectile_crosshair",
    "loot_player",
    "loot_crosshair",
};

SRWLOCK g_lock{SRWLOCK_INIT};
Keybinds g_keybinds{};
core::path::Buffer g_path{};
bool g_pathResolved{};

[[nodiscard]] bool valid(const Keybinds& keybinds) noexcept {
    for (const std::uint32_t key : keybinds.virtualKeys) {
        if (key > kMaximumVirtualKey) {
            return false;
        }
    }
    return true;
}

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=spawn_keybinds stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool parse_key(std::string_view document,
                             std::string_view name,
                             std::uint32_t& output) noexcept {
    std::array<char, 48> quoted{};
    const int length =
        std::snprintf(quoted.data(), quoted.size(), "\"%.*s\"", static_cast<int>(name.size()), name.data());
    if (length <= 0 || static_cast<std::size_t>(length) >= quoted.size()) {
        return false;
    }
    const std::size_t at = document.find(std::string_view(quoted.data(), static_cast<std::size_t>(length)));
    const std::size_t colon = at == std::string_view::npos ? at : document.find(':', at + length);
    if (colon == std::string_view::npos) {
        return false;
    }
    const char* begin = document.data() + colon + 1;
    const char* const end = document.data() + document.size();
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    const auto parsed = std::from_chars(begin, end, output, 10);
    return parsed.ec == std::errc{};
}

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
    std::array<char, kFileCapacity> document{};
    DWORD read = 0;
    const bool readOk = ReadFile(file,
                                 document.data(),
                                 static_cast<DWORD>(document.size() - 1),
                                 &read,
                                 nullptr)
                        != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0) {
        return;
    }
    Keybinds parsed{};
    const std::string_view text(document.data(), read);
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        (void)parse_key(text, kNames[index], parsed.virtualKeys[index]);
    }
    if (valid(parsed)) {
        g_keybinds = parsed;
    } else {
        report_fail("range");
    }
}

[[nodiscard]] bool store(const Keybinds& keybinds) noexcept {
    if (!g_pathResolved) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(
        document.data(),
        document.size(),
        "{\n  \"main_player\": %u,\n  \"main_crosshair\": %u,\n"
        "  \"projectile_player\": %u,\n  \"projectile_crosshair\": %u,\n"
        "  \"loot_player\": %u,\n  \"loot_crosshair\": %u\n}\n",
        static_cast<unsigned>(keybinds.virtualKeys[0]),
        static_cast<unsigned>(keybinds.virtualKeys[1]),
        static_cast<unsigned>(keybinds.virtualKeys[2]),
        static_cast<unsigned>(keybinds.virtualKeys[3]),
        static_cast<unsigned>(keybinds.virtualKeys[4]),
        static_cast<unsigned>(keybinds.virtualKeys[5]));
    if (size <= 0 || static_cast<std::size_t>(size) >= document.size()) {
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
    bool complete = WriteFile(file,
                              document.data(),
                              static_cast<DWORD>(size),
                              &written,
                              nullptr)
                        != FALSE
                    && written == static_cast<DWORD>(size);
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = {};
    g_pathResolved = core::path::artifact_directory(module, g_path)
                     && core::path::append(g_path, kFileSuffix);
    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = {};
    g_path = {};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Keybinds get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Keybinds snapshot = g_keybinds;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

bool publish(const Keybinds& keybinds) noexcept {
    if (!valid(keybinds)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = keybinds;
    const bool stored = store(keybinds);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

} // namespace sunrise::client::spawn
