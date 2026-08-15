#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * `World_CheckActivityBubbles`* @ `0x7FF74208DB80`. Matched from its prologue through the activity
 * object load and the handle shift, which is unique in the image.
 */
constexpr std::string_view kCheckSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 79 10 41 8B C0 "
    "41 8B D8 C1 F8 0D";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kCheckSignature =
    signature<signature_length(kCheckSignatureText)>(kCheckSignatureText);

/**
 * Slot written into the container. Any non-zero value pins the record. 1 is what the game's own
 * activity-swap path passes, so nothing downstream sees a number it cannot make itself.
 */
constexpr std::int32_t kRemoteSlot = 1;

/** Returned when the trampoline is gone. The caller reads it as "not armed" and rolls back. */
constexpr std::uint8_t kNotArmed = 0;

/** Lines allowed per run. The check runs once per activity container. */
constexpr unsigned kMaxReports = 4;

/** Size of one forcing line, set by its slot fields. */
constexpr std::size_t kLineCapacity = 96;

using CheckBubbles =
    std::uint8_t(__fastcall*)(void*, void*, std::int32_t, void*, std::int64_t, std::int32_t);

hooking::detour::Handle g_handle{};
std::atomic<CheckBubbles> g_original{nullptr};
std::atomic<unsigned> g_reported{0};

/**
 * Emits one forcing event while the per-run budget lasts.
 * @param slot The slot the caller passed.
 */
void report(std::int32_t slot) noexcept {
    // One atomic claim per line, so a concurrent check cannot reuse a budget slot.
    if (g_reported.fetch_add(1, std::memory_order_relaxed) >= kMaxReports) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=owner_slot result=forced was=%d now=%d",
                                      static_cast<int>(slot),
                                      static_cast<int>(kRemoteSlot));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Passes a non-zero owner activity slot into the roster container.
 * Argument 6 is the only writer of the slot, at `0x7FF74208DC13`. Every other argument is passed
 * on untouched, including argument 5, whose own arm has nothing to do with the record.
 * @return The check's own result, or the not-armed result when the trampoline is gone.
 */
__declspec(noinline) std::uint8_t __fastcall check(void* container,
                                                   void* reporter,
                                                   std::int32_t activity,
                                                   void* prefix,
                                                   std::int64_t roleIsLocal,
                                                   std::int32_t slot) noexcept {
    const CheckBubbles original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return kNotArmed;
    }
    if (!core::settings::get().client.pinReplicatedRecord) {
        return original(container, reporter, activity, prefix, roleIsLocal, slot);
    }
    if (slot != kRemoteSlot) {
        report(slot);
    }
    return original(container, reporter, activity, prefix, roleIsLocal, kRemoteSlot);
}

} // namespace

/** Attaches the owner activity slot force. */
bool install_owner_activity_slot() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kCheckSignature, "check_activity_bubbles");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=owner_slot result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&check)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=owner_slot result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<CheckBubbles>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=owner_slot result=ok");
    return true;
}

/** Detaches the owner activity slot force. */
void uninstall_owner_activity_slot() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
