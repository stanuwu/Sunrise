#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The character sign-in step's enter handler. Anchored on its two setup stores, which write the
 * step's latch field and the paired setup words, so it cannot match a sibling step's handler.
 */
constexpr std::string_view kEnterSignatureText =
    "40 53 48 83 EC ? 48 8B D9 C7 41 38 FF FF FF FF 66 C7 41 3C 00 00 33 D2";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kEnterSignature =
    signature<signature_length(kEnterSignatureText)>(kEnterSignatureText);

/** Fields of the boot step this hook touches, as byte offsets from the step's own base. */
struct StepLayout {
    /**
     * The stay flag. The exit gate needs it non-zero to stay on the select screen. The enter
     * handler clears it, so the step would skip selection on its first frame.
     */
    static constexpr std::size_t stayFlag = 72;
};

/** Value the step's own listener writes to hold the screen. Matched here. */
constexpr std::uint8_t kHold = 1;

using EnterHandler = void(__fastcall*)(std::byte*);

hooking::detour::Handle g_handle{};
std::atomic<EnterHandler> g_original{nullptr};
std::atomic_bool g_reported{false};

/**
 * Holds the character-select screen after the step sets itself up.
 * Runs once per boot on the boot-step thread, after the original writes its defaults, so the
 * original cannot overwrite this write.
 * @param step Borrowed boot-step base pointer.
 */
__declspec(noinline) void __fastcall enter_handler(std::byte* step) noexcept {
    const EnterHandler original = g_original.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(step);
    }
    if (step == nullptr) {
        return;
    }
    std::memcpy(step + StepLayout::stayFlag, &kHold, sizeof kHold);
    if (!g_reported.exchange(true, std::memory_order_relaxed)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=bootflow stage=character_select result=held");
    }
}

} // namespace

/**
 * Attaches the character-select hold.
 * @return True when the target is found and the detour attaches.
 */
bool install_character_select_hold() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kEnterSignature, "character_signin_enter");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=character_select result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&enter_handler)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=character_select result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<EnterHandler>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=character_select result=ok");
    return true;
}

/** Detaches the character-select hold. */
void uninstall_character_select_hold() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
