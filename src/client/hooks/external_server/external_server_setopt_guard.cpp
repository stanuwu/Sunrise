/**
 * Guard on `curl_easy_setopt`, attached only in external-server mode. It forces the pinned TLS
 * peer checks off, and rewrites every request URL's host to the external server. The rewrite is
 * the catch-all for request paths the HTTP executor never sees.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"
#include "route.h"

namespace sunrise::client::hooks::external_server {
namespace {

using Setopt = int (*)(void*, int, void*) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Setopt> g_original{nullptr};

/** Size of one report line: two URLs. */
constexpr std::size_t kReportCapacity = 2 * kUrlCapacity;

/** @param option Forced curl option id. */
void report_unpin(int option) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=external stage=unpin option=%d value=0 result=ok", option);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one URL decision with both forms, so a missed rewrite names its own host. */
void report_url(std::string_view from, std::string_view to) noexcept {
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=external stage=setopt_url from=%.*s to=%.*s result=%s",
                                      static_cast<int>(from.size()),
                                      from.data(),
                                      static_cast<int>(to.size()),
                                      to.data(),
                                      to.empty() ? "unchanged" : "ok");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Rewrites the caller's URL into storage that outlives this call on this thread.
 * curl copies the string inside setopt, but only after the replacement returns.
 * @param value Caller-owned null-terminated URL.
 * @return Replacement URL, or the caller's own when it was not rewritten.
 */
[[nodiscard]] void* redirect_url(void* value) noexcept {
    thread_local std::array<char, kUrlCapacity> held{};
    const auto* const text = static_cast<const char*>(value);
    const std::string_view original(text, strnlen_s(text, kUrlCapacity));
    const std::size_t size = rewrite_host(original, held);
    report_url(original, {held.data(), size});
    return size != 0 ? held.data() : value;
}

/**
 * Forces the peer-verification options to zero, points URLs at the external server, and passes
 * everything else through.
 * @param handle Borrowed curl easy handle.
 * @param option curl option id.
 * @param value Caller-supplied option value.
 * @return The original result, or CURLE_FAILED_INIT when there is no trampoline.
 */
__declspec(noinline) int setopt(void* handle, int option, void* value) noexcept {
    const Setopt original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return 2; // CURLE_FAILED_INIT
    }
    if (option == kSslVerifyPeer || option == kSslVerifyHost) {
        report_unpin(option);
        return original(handle, option, nullptr);
    }
    if (option == kUrl && value != nullptr) {
        return original(handle, option, redirect_url(value));
    }
    return original(handle, option, value);
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail_install(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=external stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Attaches the setopt guard when the external-server switch is on. */
bool install() noexcept {
    if (!enabled() || g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kSetoptSignature, "curl_easy_setopt");
    if (target == nullptr) {
        return fail_install("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&setopt)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail_install("attach");
    }
    g_original.store(reinterpret_cast<Setopt>(g_handle.original), std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=external stage=install result=ok");
    return true;
}

/** Detaches the setopt guard and drops its trampoline. */
void uninstall() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

/** @return True while the setopt guard is attached. */
bool is_installed() noexcept {
    return g_handle.attached;
}

} // namespace sunrise::client::hooks::external_server
