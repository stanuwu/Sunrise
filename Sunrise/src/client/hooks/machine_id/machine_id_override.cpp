#include "machine_id_override.h"

#include <Windows.h>

#include <array>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "machine_id_cache.h"

namespace sunrise::client::hooks::machine_id {
namespace {
// The game derives one machine id per PC and peers key each other by it, so two clients on
// the same PC would collide. An optional `client.machine_id` replaces the cached value; the
// override is a no-op when the setting is absent.
// The verified native call site names this cache, its validity byte and its composer.
constexpr std::uintptr_t kFlagRva = 0x20D4A60;
constexpr std::uintptr_t kRecordRva = 0x20D4A61;
constexpr std::uintptr_t kIdRva = 0x20D4A67;
constexpr std::uintptr_t kComposerRva = 0x3000C0;
constexpr std::uintptr_t kSiteRva = 0x2FFDA1;
constexpr std::array<std::uint8_t, 35> kSiteBytes{
    0x80, 0x3D, 0xB8, 0x4C, 0xDD, 0x01, 0x00, 0x75, 0x1A, 0x48, 0x8D, 0x15,
    0xB6, 0x4C, 0xDD, 0x01, 0x48, 0x8D, 0x0D, 0xA9, 0x4C, 0xDD, 0x01, 0xE8,
    0x03, 0x03, 0x00, 0x00, 0xC6, 0x05, 0x9C, 0x4C, 0xDD, 0x01, 0x01};
SRWLOCK g_lock = SRWLOCK_INIT;
cache::Override g_override;
bool g_reportedFailure{};

bool matches(void* module) noexcept {
    if (!module) {
        return false;
    }
    __try {
        return std::memcmp(
                   static_cast<std::byte*>(module) + kSiteRva, kSiteBytes.data(), kSiteBytes.size())
               == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write(void* destination, const void* source, std::size_t size) noexcept {
    DWORD protection{};
    if (!VirtualProtect(destination, size, PAGE_READWRITE, &protection)) {
        return false;
    }
    std::memcpy(destination, source, size);
    DWORD unused{};
    if (!VirtualProtect(destination, size, protection, &unused)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=machine_id stage=protection result=restore_failed");
    }
    // The write took effect, so its owner must retain the original for shutdown restoration.
    return true;
}
} // namespace

bool install(void* gameModule) noexcept {
    const auto requested = core::settings::get().client.machineId;
    if (!requested) {
        return true;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_override.active) {
        const bool same = g_override.requested == requested;
        ReleaseSRWLockExclusive(&g_lock);
        return same;
    }
    bool installed = false;
    if (matches(gameModule)) {
        auto* base = static_cast<std::byte*>(gameModule);
        const cache::Fields fields{
            reinterpret_cast<std::uint8_t*>(base + kFlagRva), base + kRecordRva, base + kIdRva};
        installed = cache::install(fields,
                                   reinterpret_cast<cache::Compose>(base + kComposerRva),
                                   &write,
                                   requested,
                                   g_override);
    }
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::error,
                     installed ? "ev=machine_id stage=install result=ok"
                               : "ev=machine_id stage=install result=fail");
    return installed;
}
void poll() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool failed = !cache::poll(g_override, &write);
    const bool report = failed && !g_reportedFailure;
    g_reportedFailure = failed;
    ReleaseSRWLockExclusive(&g_lock);
    if (report) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=machine_id stage=refresh result=write_failed");
    }
}
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool removed = cache::uninstall(g_override, &write);
    if (removed) {
        g_reportedFailure = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}
} // namespace sunrise::client::hooks::machine_id
