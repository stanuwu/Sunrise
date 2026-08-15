#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The matchmaking composition check. Its prologue repeats across the image, so the pattern runs
 * on to the properties byte read, which is unique to this function. Every branch displacement is
 * wildcarded.
 */
constexpr std::string_view kCheckSignatureText =
    "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 48 85 C9 0F 84 ? ? ? ? 48 85 D2 0F 84 ? ? ? ? "
    "0F B6 82 A2 02 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kCheckSignature =
    signature<signature_length(kCheckSignatureText)>(kCheckSignatureText);

/** Fields of the composition properties this hook touches, as byte offsets from their base. */
struct PropertiesLayout {
    /**
     * Big fireteam count, signed 32-bit. The check fails the composition when it is over the set
     * cap. Both operands are local, so only this field can change the result.
     */
    static constexpr std::size_t bigFireteamCount = 20;
};

/** Count that clears the cap comparison for every configured cap. */
constexpr std::int32_t kSolo = 0;

/** Check result meaning an argument was null; also returned when the trampoline is gone. */
constexpr std::int64_t kNullArgument = 3;

/**
 * Lines allowed per run. The check runs on every composition test, so an uncapped report buries
 * the rest of the log. This budget still shows the count the boot started with.
 */
constexpr unsigned kMaxReports = 4;

/** Size of one zeroing line, set by its stage and count fields. */
constexpr std::size_t kLineCapacity = 96;

using Check = std::int64_t(__fastcall*)(void*, std::byte*);

hooking::detour::Handle g_handle{};
std::atomic<Check> g_original{nullptr};
std::atomic<unsigned> g_reported{0};

/**
 * Emits one zeroing event while the per-run budget lasts.
 * @param count The count that was replaced.
 */
void report(std::int32_t count) noexcept {
    // One atomic claim per line, so a concurrent check cannot reuse a budget slot.
    if (g_reported.fetch_add(1, std::memory_order_relaxed) >= kMaxReports) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=composition result=zeroed count=%d",
                                      static_cast<int>(count));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Clears the big fireteam count so the composition check passes.
 * The write repeats on every call because the field's producer rewrites it each run.
 * Nothing between entry and the compare rebuilds it, so the entry write is the one compared.
 * @param config Borrowed composition config, passed through untouched.
 * @param props Borrowed composition properties whose count is cleared.
 * @return The check's own result, or the null-argument result when the trampoline is gone.
 */
__declspec(noinline) std::int64_t __fastcall check(void* config, std::byte* props) noexcept {
    const Check original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return kNullArgument;
    }
    if (props == nullptr) {
        return original(config, props);
    }
    std::int32_t count = 0;
    std::memcpy(&count, props + PropertiesLayout::bigFireteamCount, sizeof count);
    std::memcpy(props + PropertiesLayout::bigFireteamCount, &kSolo, sizeof kSolo);
    if (count != kSolo) {
        report(count);
    }
    return original(config, props);
}

} // namespace

/**
 * Attaches the solo composition fix.
 * @return True when the target is found and the detour attaches.
 */
bool install_composition_check() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kCheckSignature, "matchmaking_composition");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=composition result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&check)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=composition result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<Check>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=composition result=ok");
    return true;
}

/** Detaches the solo composition fix. */
void uninstall_composition_check() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
