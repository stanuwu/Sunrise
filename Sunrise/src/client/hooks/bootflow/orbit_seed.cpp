#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/build_data/runtime.h"
#include "bootflow_hook_lifecycle.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

constexpr std::string_view kSeedSignatureText = "E8 ? ? ? ? 4C 8B C8 BF C5 9D 1C 81";
constexpr auto kSeedSignature =
    signature<signature_length(kSeedSignatureText)>(kSeedSignatureText);

constexpr std::size_t kSeedImmediateOffset = 9;
constexpr std::size_t kDefaultLoadOffset = 0x43;
constexpr std::size_t kDefaultLoadSize = 3;
constexpr std::array<std::byte, kDefaultLoadSize> kDefaultLoadExpected{
    std::byte{0x8B}, std::byte{0x79}, std::byte{0x14}};
constexpr std::array<std::byte, kDefaultLoadSize> kDefaultLoadPatch{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};

constexpr std::wstring_view kMapFileSuffix = L"\\orbit_map.txt";
constexpr std::size_t kMapFileCapacity = 64 * 1024;
constexpr std::size_t kMapCapacity = 512;
constexpr std::size_t kDestinationCapacity = 40;
constexpr std::size_t kOrbitNameCapacity = 48;
constexpr std::uint32_t kHashBasis = 0x811C9DC5U;
constexpr std::uint32_t kHashPrime = 0x01000193U;
constexpr std::string_view kOrbitPrefix = "orbit";

struct MapEntry {
    std::array<char, kDestinationCapacity> destination{};
    std::uint8_t destinationLength{};
    std::uint32_t orbitHash{};
};

std::array<MapEntry, kMapCapacity> g_map{};
std::size_t g_mapCount = 0;
std::uint32_t g_defaultHash = 0;
std::uint32_t g_applied = 0;
bool g_listed = false;
SRWLOCK g_lock = SRWLOCK_INIT;

std::byte* g_immediate = nullptr;
std::byte* g_defaultLoad = nullptr;
std::uint32_t g_originalSeed = 0;
std::array<std::byte, kDefaultLoadSize> g_originalLoad{};

[[nodiscard]] bool write_bytes(std::byte* address, const void* source, std::size_t size) noexcept {
    DWORD previous = 0;
    if (VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous) == FALSE) {
        return false;
    }
    std::memcpy(address, source, size);
    DWORD restored = 0;
    const bool reset = VirtualProtect(address, size, previous, &restored) != FALSE;
    (void)FlushInstructionCache(GetCurrentProcess(), address, size);
    return reset;
}

[[nodiscard]] std::uint32_t hash_of(std::string_view text) noexcept {
    std::uint32_t value = kHashBasis;
    for (const char character : text) {
        value = (value * kHashPrime) ^ static_cast<std::uint8_t>(character);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto blank = [](char value) noexcept {
        return value == ' ' || value == '\t' || value == '\r';
    };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool lowercase_name(std::string_view text, std::size_t limit,
                                  std::array<char, kOrbitNameCapacity>& output,
                                  std::uint8_t& length) noexcept {
    output = {};
    length = 0;
    if (text.empty() || text.size() > limit) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        char value = text[index];
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
        const bool usable = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')
                            || value == '_';
        if (!usable) {
            return false;
        }
        output[index] = value;
    }
    length = static_cast<std::uint8_t>(text.size());
    return true;
}

void load_map(void* module) noexcept {
    g_mapCount = 0;
    core::path::Buffer file;
    if (!core::path::artifact_directory(module, file)
        || !core::path::append(file, kMapFileSuffix)) {
        return;
    }
    const HANDLE handle = CreateFileW(file.chars.data(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    static std::array<char, kMapFileCapacity> document{};
    LARGE_INTEGER measured{};
    DWORD read = 0;
    const bool sized = GetFileSizeEx(handle, &measured) != FALSE && measured.QuadPart >= 0
                       && static_cast<std::uint64_t>(measured.QuadPart) <= document.size();
    const auto wanted = sized ? static_cast<DWORD>(measured.QuadPart) : 0;
    const bool complete =
        sized
        && (wanted == 0
            || (ReadFile(handle, document.data(), wanted, &read, nullptr) != FALSE
                && read == wanted));
    (void)CloseHandle(handle);
    if (!complete) {
        return;
    }

    const std::string_view text(document.data(), read);
    std::size_t start = 0;
    std::size_t rejected = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '\n') {
            continue;
        }
        const std::string_view line = trim(text.substr(start, index - start));
        start = index + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t split = line.find('=');
        if (split == std::string_view::npos || g_mapCount >= g_map.size()) {
            ++rejected;
            continue;
        }
        std::array<char, kOrbitNameCapacity> destination{};
        std::array<char, kOrbitNameCapacity> orbit{};
        std::uint8_t destinationLength = 0;
        std::uint8_t orbitLength = 0;
        if (!lowercase_name(trim(line.substr(0, split)), kDestinationCapacity, destination,
                            destinationLength)
            || !lowercase_name(trim(line.substr(split + 1)), kOrbitNameCapacity, orbit,
                               orbitLength)) {
            ++rejected;
            continue;
        }
        MapEntry& entry = g_map[g_mapCount++];
        entry = {};
        std::copy_n(destination.begin(), destinationLength, entry.destination.begin());
        entry.destinationLength = destinationLength;
        entry.orbitHash = hash_of({orbit.data(), orbitLength});
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=orbit_map pairs=%zu rejected=%zu",
                                      g_mapCount,
                                      rejected);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void list_stems() noexcept {
    if (g_listed || !state::build_data::scenario_layouts_ready()) {
        return;
    }
    g_listed = true;
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&list_stems),
                           &self)
        == FALSE) {
        return;
    }
    core::path::Buffer file;
    if (!core::path::artifact_directory(self, file)
        || !core::path::append(file, L"\\orbit_stems.txt")) {
        return;
    }
    static std::array<state::build_data::scenarios::Definition,
                      state::build_data::scenarios::kDefinitionCapacity>
        layouts{};
    std::size_t count = 0;
    if (!state::build_data::snapshot_scenario_layouts(layouts, count)) {
        return;
    }
    const HANDLE handle = CreateFileW(file.chars.data(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    std::array<char, 256> row{};
    for (std::size_t index = 0; index < count; ++index) {
        const state::build_data::scenarios::Definition& layout = layouts[index];
        const int written = std::snprintf(row.data(),
                                          row.size(),
                                          "%.*s = %.*s\r\n",
                                          static_cast<int>(layout.nameLength),
                                          layout.name.data(),
                                          static_cast<int>(layout.spawnStemLength),
                                          layout.spawnStem.data());
        if (written <= 0) {
            continue;
        }
        DWORD wrote = 0;
        (void)WriteFile(handle, row.data(), static_cast<DWORD>(written), &wrote, nullptr);
    }
    (void)CloseHandle(handle);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=orbit_stems rows=%zu",
                                      count);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool match_name(std::string_view candidate, std::uint32_t& hash) noexcept {
    std::array<char, kOrbitNameCapacity> wanted{};
    std::uint8_t length = 0;
    if (!lowercase_name(candidate, kDestinationCapacity, wanted, length)) {
        return false;
    }
    const std::string_view name(wanted.data(), length);
    std::size_t best = 0;
    for (std::size_t index = 0; index < g_mapCount; ++index) {
        const MapEntry& entry = g_map[index];
        const std::string_view key(entry.destination.data(), entry.destinationLength);
        if (key.size() > name.size() || name.compare(0, key.size(), key) != 0) {
            continue;
        }
        if (key.size() != name.size() && name[key.size()] != '_') {
            continue;
        }
        if (key.size() > best) {
            best = key.size();
            hash = entry.orbitHash;
        }
    }
    return best != 0;
}

[[nodiscard]] std::string_view stem_of(std::string_view packageName) noexcept {
    static state::build_data::scenarios::Definition layout{};
    layout = {};
    if (!state::build_data::find_scenario_layout(packageName, layout)) {
        return {};
    }
    return {layout.spawnStem.data(), layout.spawnStemLength};
}

void report(std::uint32_t hash, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=orbit_seed hash=0x%08X result=%s",
                                      hash,
                                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         std::string_view(result) == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

bool install_orbit_seed() noexcept {
    if (g_immediate != nullptr) {
        return true;
    }
    const std::uint32_t hash = core::settings::get().client.orbitSliceSetHash;
    if (hash == 0) {
        return true;
    }
    std::byte* const match = scan_main_image_unique(kSeedSignature, "orbit_slice_set_seed");
    if (match == nullptr) {
        report(hash, "target");
        return false;
    }
    std::byte* const immediate = match + kSeedImmediateOffset;
    std::byte* const defaultLoad = match + kDefaultLoadOffset;
    std::array<std::byte, kDefaultLoadSize> present{};
    std::memcpy(present.data(), defaultLoad, present.size());
    if (present != kDefaultLoadExpected) {
        report(hash, "shape");
        return false;
    }
    std::uint32_t previousSeed = 0;
    std::memcpy(&previousSeed, immediate, sizeof previousSeed);
    if (!write_bytes(immediate, &hash, sizeof hash)) {
        report(hash, "write");
        return false;
    }
    if (!write_bytes(defaultLoad, kDefaultLoadPatch.data(), kDefaultLoadPatch.size())) {
        (void)write_bytes(immediate, &previousSeed, sizeof previousSeed);
        report(hash, "write");
        return false;
    }
    g_immediate = immediate;
    g_defaultLoad = defaultLoad;
    g_originalSeed = previousSeed;
    g_originalLoad = present;
    g_defaultHash = hash;
    g_applied = hash;
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install_orbit_seed),
                           &self)
        != FALSE) {
        load_map(self);
    }
    report(hash, "ok");
    return true;
}

void note_destination(std::string_view packageName) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_immediate == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    list_stems();
    if (packageName.size() >= kOrbitPrefix.size()
        && packageName.compare(0, kOrbitPrefix.size(), kOrbitPrefix) == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const std::string_view stem = stem_of(packageName);
    std::uint32_t wanted = g_defaultHash;
    const char* source = "default";
    if (match_name(packageName, wanted)) {
        source = "name";
    } else if (!stem.empty() && match_name(stem, wanted)) {
        source = "stem";
    }
    bool wrote = false;
    if (wanted != 0 && wanted != g_applied && write_bytes(g_immediate, &wanted, sizeof wanted)) {
        g_applied = wanted;
        wrote = true;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=orbit_follow from=%.*s stem=%.*s "
                                      "hash=0x%08X source=%s changed=%u",
                                      static_cast<int>(packageName.size()),
                                      packageName.data(),
                                      static_cast<int>(stem.size()),
                                      stem.data(),
                                      wanted,
                                      source,
                                      wrote ? 1U : 0U);
    ReleaseSRWLockExclusive(&g_lock);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void uninstall_orbit_seed() noexcept {
    if (g_immediate == nullptr) {
        return;
    }
    (void)write_bytes(g_defaultLoad, g_originalLoad.data(), g_originalLoad.size());
    (void)write_bytes(g_immediate, &g_originalSeed, sizeof g_originalSeed);
    g_immediate = nullptr;
    g_defaultLoad = nullptr;
    g_originalSeed = 0;
    g_originalLoad = {};
    g_mapCount = 0;
    g_defaultHash = 0;
    g_applied = 0;
    g_listed = false;
}

} // namespace sunrise::client::hooks::bootflow
