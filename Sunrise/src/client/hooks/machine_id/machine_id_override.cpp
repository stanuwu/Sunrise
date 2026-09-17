#include "machine_id_override.h"

#include <Windows.h>

#include <array>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../patterns/image_scan.h"
#include "machine_id_cache.h"

namespace sunrise::client::hooks::machine_id {
namespace {
// The game derives one machine id per PC and peers key each other by it, so two clients on
// the same PC would collide. An optional `client.machine_id` replaces the cached value; the
// override is a no-op when the setting is absent.
SRWLOCK g_lock = SRWLOCK_INIT;
cache::Override g_override;
bool g_reportedFailure{};

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

bool install() noexcept {
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
    using namespace patterns;
    constexpr std::string_view text = "80 3D ? ? ? ? 00 75 1A 48 8D 15 ? ? ? ? 48 8D 0D ? ? ? ? "
                                      "E8 ? ? ? ? C6 05 ? ? ? ? 01";
    constexpr auto pattern = signature<signature_length(text)>(text);
    if (auto* site = scan_main_image_unique(pattern, "machine_identity_cache")) {
        // CMP and MOV must name the same validity byte. LEA RDX/RCX supply the ID/record;
        // the relative CALL supplies their native composer.
        auto* initialized = resolve_relative(site + 2, site + 7);
        if (initialized == resolve_relative(site + 30, site + 35)) {
            const cache::Fields fields{reinterpret_cast<std::uint8_t*>(initialized),
                                       resolve_relative(site + 19, site + 23),
                                       resolve_relative(site + 12, site + 16)};
            installed = cache::install(
                fields,
                reinterpret_cast<cache::Compose>(resolve_relative(site + 24, site + 28)),
                &write,
                requested,
                g_override);
        }
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
