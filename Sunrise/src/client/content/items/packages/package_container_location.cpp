#include <Windows.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/filesystem/temporary_sibling.h"
#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../state/content/content_catalog.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game.h"
#include "build.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Installed packages sit beside the main executable. */
constexpr std::wstring_view kPackageDirectory = L"packages";
/** Compact name id of the container that names every investment definition table. */
constexpr std::uint32_t kInvestmentGlobalsNameHash = 0x6F7125CBU;
/** Studio reads this one opt-in export below the Sunrise artifact directory. */
constexpr std::wstring_view kKeyExportPath = L"exports\\package_keys.bin";
/** A sibling keeps partial writes out of the published export. */
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** Identifies the versioned package-key export, not key material. */
constexpr std::array kKeyExportMagic{'S', 'R', 'P', 'K', 'K', 'E', 'Y', 'S'};
/** Version 1 stores a build id, two AES-128 keys, and a 12-byte nonce. */
constexpr std::uint32_t kKeyExportVersion = 1;
/** The package reader and runtime signatures target the installed build 86657. */
constexpr std::uint32_t kKeyExportBuild = 86657;
/** Version 1 is exactly 60 bytes, with no padding between its fields. */
constexpr std::size_t kKeyExportSize = 60;

/** Studio reads the integer fields in little-endian order. */
struct KeyExport {
    std::array<char, kKeyExportMagic.size()> magic{kKeyExportMagic};
    std::uint32_t version{kKeyExportVersion};
    std::uint32_t build{kKeyExportBuild};
    reader::BlockKeys keys{};
};

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(KeyExport) == kKeyExportSize);

/**
 * Replaces the export only after its complete payload reaches disk.
 * @param payload Versioned keys borrowed for this write.
 * @return True when Studio can read the complete new export.
 */
[[nodiscard]] bool write_key_export(const KeyExport& payload) noexcept {
    core::path::Buffer path;
    if (!core::path::artifact_file(kKeyExportPath, path)) {
        return false;
    }
    core::path::Buffer temporaryPath = path;
    if (!core::path::append(temporaryPath, kTemporarySuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(temporaryPath.chars.data(),
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
    bool complete = WriteFile(file, &payload, sizeof payload, &written, nullptr) != FALSE
                    && written == sizeof payload && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    if (complete) {
        complete = core::path::publish_sibling(temporaryPath.chars.data(), path.chars.data());
    }
    if (!complete) {
        (void)DeleteFileW(temporaryPath.chars.data());
    }
    return complete;
}

} // namespace

/** Exports once at Client activation only when the startup setting requests it. */
void export_keys_if_requested() noexcept {
    if (!core::settings::get().client.exports.packageKeys) {
        return;
    }
    KeyExport payload;
    if (!collect_keys(payload.keys)) {
        SecureZeroMemory(&payload, sizeof payload);
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=package_keys action=export result=fail reason=unavailable");
        return;
    }
    const bool written = write_key_export(payload);
    SecureZeroMemory(&payload, sizeof payload);
    core::log::write(core::log::Channel::client,
                     written ? core::log::Level::info : core::log::Level::error,
                     written ? "ev=package_keys action=export result=ok"
                             : "ev=package_keys action=export result=fail reason=write");
}

/** Reports whether the pass can read anything yet. */
bool readable() noexcept {
    reader::BlockKeys keys{};
    const bool available = collect_keys(keys);
    SecureZeroMemory(&keys, sizeof keys);
    return available;
}

/** Copies the block key material this pass borrows. */
bool collect_keys(reader::BlockKeys& keys) noexcept {
    keys = {};
    const state::SignOnState& signOn = state::sign_on();
    targets::game::packages::KeyTable table{};
    if (!signOn.bootstrapTokenPresent) {
        return false;
    }
    const bool read = targets::game::packages::read(table);
    if (!read) {
        SecureZeroMemory(&table, sizeof table);
        return false;
    }
    keys.alternate = table.alternateKey;
    keys.nonceBase = table.nonceBase;
    // The primary key comes straight from the bootstrap token and the installed identity
    // constant, byte by byte, without reading a native derived-identity buffer.
    for (std::size_t index = 0; index < keys.primary.size(); ++index) {
        const auto tokenByte = static_cast<unsigned char>(signOn.bootstrapToken[index]);
        const auto constantByte = static_cast<unsigned char>(table.identityConstant[index]);
        keys.primary[index] = static_cast<std::byte>(tokenByte + constantByte);
    }
    SecureZeroMemory(&table, sizeof table);
    return true;
}

/** Collects every catalogue tag carrying the container name. */
bool investment_globals_tags(std::array<std::uint32_t, kContainerCandidates>& candidates,
                             std::size_t& count) noexcept {
    candidates = {};
    count = 0;
    std::array<state::content::Definition, kContainerCandidates> matches{};
    std::size_t matched = 0;
    if (!state::content::lookup_hash(kInvestmentGlobalsNameHash, matches, matched)) {
        return false;
    }
    for (std::size_t index = 0; index < matched; ++index) {
        if (matches[index].tag != 0) {
            candidates[count++] = matches[index].tag;
        }
    }
    return count != 0;
}

/** @param directory Receives the installed packages directory. @return True when it exists. */
bool package_directory(core::path::Buffer& directory) noexcept {
    if (!core::path::module_directory(GetModuleHandleW(nullptr), directory)
        || !core::path::append(directory, kPackageDirectory)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.chars.data());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace sunrise::client::content::items::packages
