#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The destination-hold predicate of the orbit setup step. Its prologue repeats across the image,
 * so the pattern runs on through the call and the flag test that follow. Every displacement is
 * wildcarded.
 */
constexpr std::string_view kHoldSignatureText =
    "48 89 5C 24 ? 57 48 83 EC ? 48 8B D9 E8 ? ? ? ? 80 3D ? ? ? ? 00 48 8B F8 75 ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kHoldSignature = signature<signature_length(kHoldSignatureText)>(kHoldSignatureText);

/**
 * Answer that lets the handoff test pass. The native predicate holds for an armed pending
 * destination, or for a cinematic under a 5,000 ms timer. This answer skips that wait. It has one
 * call site, the step's own update, so nothing else sees the change.
 */
constexpr bool kReleased = false;

hooking::detour::Handle g_handle{};
std::atomic_bool g_reported{false};

/**
 * Releases the destination hold. The original is never called: blocking is the only answer it
 * gives here, so answering directly gives the same result with no call.
 * @param stepCtx Borrowed step context; the answer does not depend on it.
 * @return The released answer, always.
 */
__declspec(noinline) bool __fastcall destination_hold(void* stepCtx) noexcept {
    (void)stepCtx;
    if (!g_reported.exchange(true, std::memory_order_relaxed)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=bootflow stage=orbit_handoff result=released");
    }
    return kReleased;
}

} // namespace

/**
 * Attaches the orbit handoff release.
 * @return True when the target is found and the detour attaches.
 */
bool install_orbit_handoff() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kHoldSignature, "orbit_destination_hold");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=orbit_handoff result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&destination_hold)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=orbit_handoff result=fail reason=attach");
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=orbit_handoff result=ok");
    return true;
}

/** Detaches the orbit handoff release. */
void uninstall_orbit_handoff() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_reported.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
