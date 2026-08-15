#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "internal.h"

namespace sunrise::client::hooks::queuez {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * The update-notification handler, reached only through an indirect method dispatch, so it has no
 * call site to anchor on. Matched on its own prologue, stack-cookie store and first message read.
 */
constexpr std::string_view kHandlerSignatureText =
    "40 53 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 41 "
    "8B 08";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kHandlerSignature =
    signature<signature_length(kHandlerSignatureText)>(kHandlerSignatureText);

/** Fields of the dispatched message record, as byte offsets from its base. */
struct MessageLayout {
    /** Message identity, read by the handler before the payload. */
    static constexpr std::size_t id = 0;
    /** Payload pointer. The handler reads through it with no null check. */
    static constexpr std::size_t payload = 8;
};

/** Verdict returned for a skipped message. It is the handler's own no-op result. */
constexpr char kNotHandled = 0;

using Handler = char(__fastcall*)(void*, void*, std::byte*);

hooking::detour::Handle g_handle{};
std::atomic<Handler> g_original{nullptr};
std::atomic_bool g_reported{false};

/**
 * Drops an update notification whose payload is null. When the source object holds no entries, the
 * emit leaves the descriptor pair zeroed and the handler reads through it anyway. That fault
 * unwinds out of the dispatching tick, so the message is skipped here.
 * @param self Borrowed handler instance.
 * @param context Borrowed dispatch context.
 * @param message Borrowed message record, or null.
 * @return The handler's own result, or the not-handled verdict for a skipped message.
 */
__declspec(noinline) char __fastcall handler(void* self,
                                             void* context,
                                             std::byte* message) noexcept {
    std::uintptr_t payload = 0;
    if (message != nullptr) {
        std::memcpy(&payload, message + MessageLayout::payload, sizeof payload);
    }
    if (payload == 0) {
        if (!g_reported.exchange(true, std::memory_order_relaxed)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=queuez stage=notification result=skipped reason=null_payload");
        }
        return kNotHandled;
    }
    const Handler original = g_original.load(std::memory_order_acquire);
    return original != nullptr ? original(self, context, message) : kNotHandled;
}

} // namespace

/**
 * Attaches the update-notification null-payload guard.
 * @return True when the target is found and the detour attaches.
 */
bool install_null_payload_guard() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kHandlerSignature, "queuez_update_handler");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=queuez stage=notification result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&handler)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=queuez stage=notification result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<Handler>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=queuez stage=notification result=ok");
    return true;
}

/** Detaches the null-payload guard. */
void uninstall_null_payload_guard() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(false, std::memory_order_release);
}

/** @return True while the null-payload guard is attached. */
bool null_payload_guard_installed() noexcept {
    return g_handle.attached;
}

} // namespace sunrise::client::hooks::queuez
