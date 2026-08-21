/** Godmode. Replaces the damage value instruction while enabled and restores it when disabled. */

#include "godmode.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::godmode {
namespace {

constexpr std::string_view kInstructionText = "8B 74 24 ? 48 63 58";
constexpr auto kInstruction =
    patterns::signature<patterns::signature_length(kInstructionText)>(kInstructionText);
constexpr std::array<std::byte, 4> kOriginal{
    std::byte{0x8B}, std::byte{0x74}, std::byte{0x24}, std::byte{0x38}};
constexpr std::array<std::byte, 4> kPatch{
    std::byte{0x83}, std::byte{0xCE}, std::byte{0xFF}, std::byte{0x90}};

SRWLOCK g_lock{SRWLOCK_INIT};
std::byte* g_instruction{};

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=godmode stage=patch result=fail reason=%s", reason);
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
    if (g_instruction == nullptr) {
        return false;
    }
    if (std::memcmp(g_instruction, replacement.data(), replacement.size()) == 0) {
        return true;
    }
    if (std::memcmp(g_instruction, expected.data(), expected.size()) != 0) {
        return false;
    }
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
                              g_instruction,
                              replacement.data(),
                              replacement.size(),
                              &written)
               != FALSE
           && written == replacement.size();
}

} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_instruction != nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    g_instruction = patterns::scan_main_image_unique(kInstruction, "godmode");
    if (g_instruction == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return fail("signature");
    }
    if (!write(client::player::get().godmodeEnabled)) {
        g_instruction = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        return fail("bytes");
    }
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=godmode stage=install result=ok");
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
    if (g_instruction != nullptr && !write(false)) {
        (void)fail("restore");
    }
    g_instruction = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::client::hooks::godmode
