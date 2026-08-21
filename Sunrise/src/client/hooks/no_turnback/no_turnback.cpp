/** No turnback. Changes the called check's result while enabled and restores it when disabled. */

#include "no_turnback.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::no_turnback {
namespace {

constexpr std::string_view kCallText = "E8 ? ? ? ? 0F 10 0B 84 C0";
constexpr auto kCall = patterns::signature<patterns::signature_length(kCallText)>(kCallText);
constexpr std::size_t kNearCallOperand = 1;
constexpr std::size_t kNearCallLength = 5;
constexpr std::size_t kResultOffset = 0x6F;
constexpr std::array<std::byte, 2> kOriginal{std::byte{0xB0}, std::byte{0x01}};
constexpr std::array<std::byte, 2> kPatch{std::byte{0xB0}, std::byte{0x00}};

SRWLOCK g_lock{SRWLOCK_INIT};
std::byte* g_result{};

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=no_turnback stage=patch result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Writes one validated instruction. WriteProcessMemory handles the executable page protection. */
[[nodiscard]] bool write(bool enabled) noexcept {
    const auto& expected = enabled ? kOriginal : kPatch;
    const auto& replacement = enabled ? kPatch : kOriginal;
    if (g_result == nullptr) {
        return false;
    }
    if (std::memcmp(g_result, replacement.data(), replacement.size()) == 0) {
        return true;
    }
    if (std::memcmp(g_result, expected.data(), expected.size()) != 0) {
        return false;
    }
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
                              g_result,
                              replacement.data(),
                              replacement.size(),
                              &written)
               != FALSE
           && written == replacement.size();
}

} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_result != nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    std::byte* const call = patterns::scan_main_image_unique(kCall, "no_turnback");
    auto* const target = call == nullptr
                             ? nullptr
                             : static_cast<std::byte*>(patterns::resolve_relative(
                                   call + kNearCallOperand, call + kNearCallLength));
    if (target == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return fail("signature");
    }
    g_result = target + kResultOffset;
    if (!write(client::player::get().noTurnbackEnabled)) {
        g_result = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        return fail("bytes");
    }
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=no_turnback stage=install result=ok");
    return true;
}

bool set_enabled(bool enabled) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool applied = write(enabled);
    ReleaseSRWLockExclusive(&g_lock);
    return applied || fail("write");
}

void uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_result != nullptr && !write(false)) {
        (void)fail("restore");
    }
    g_result = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::client::hooks::no_turnback
