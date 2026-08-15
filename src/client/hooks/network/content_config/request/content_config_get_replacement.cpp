#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/content/manifest/encoder.h"
#include "../../../../../state/content_manifest/content_manifest_state_runtime.h"
#include "../../../../../state/entitlements/entitlement_runtime.h"
#include "../coordinator/content_config_call_coordinator.h"
#include "../internal.h"
#include "../protocol.h"

namespace sunrise::client::hooks::network::content_config::request {
namespace {

/** The Client reads this request-status value as success. */
constexpr LONG kRequestSucceeded = 2;
/** HTTP 200 returns one complete local ContentConfig body. */
constexpr LONG kHttpOk = 200;
/** HTTP 503 finishes an unavailable request locally. */
constexpr LONG kHttpUnavailable = 503;
/** A missing result prefix cannot be finished through the Client ABI. */
constexpr std::int64_t kMissingResult = -1;
/** 1 means the async request was taken and finished locally. */
constexpr std::int64_t kCompletedResult = 1;
/** One extra byte to read, which proves the URL ends right after the route. */
constexpr std::size_t kUrlInspectionCapacity = kLocalUrl.size() + 1;

/** The exact result prefix the allocated-buffer GET completion reader uses. */
struct HttpRequestResult final {
    volatile LONG64 transferred{};
    volatile LONG pending{};
    LONG requestStatus{};
    LONG httpStatus{};
};

/** ABI offset of the pending flag in the Client result prefix. */
constexpr std::size_t kPendingOffset = 8;
/** ABI offset of the request-status field in the Client result prefix. */
constexpr std::size_t kRequestStatusOffset = 12;
/** ABI offset of the HTTP-status field in the Client result prefix. */
constexpr std::size_t kHttpStatusOffset = 16;
static_assert(offsetof(HttpRequestResult, pending) == kPendingOffset);
static_assert(offsetof(HttpRequestResult, requestStatus) == kRequestStatusOffset);
static_assert(offsetof(HttpRequestResult, httpStatus) == kHttpStatusOffset);

/** Caller-owned output, carried through the State snapshot visitor. */
struct EncodeContext final {
    std::span<std::byte> output;
    std::size_t size{};
};

/** @param result Client result prefix reset to a pending state. */
void begin_request(HttpRequestResult& result) noexcept {
    InterlockedExchange64(&result.transferred, 0);
    result.requestStatus = 0;
    result.httpStatus = 0;
    InterlockedExchange(&result.pending, 1);
}

/**
 * Writes status and size, then clears the pending flag that frees the Client reader.
 * @param result Client-owned async result prefix.
 * @param status Local HTTP status.
 * @param size Response size in bytes.
 */
void complete_request(HttpRequestResult& result, LONG status, std::size_t size) noexcept {
    result.requestStatus = kRequestSucceeded;
    result.httpStatus = status;
    InterlockedExchange64(&result.transferred, static_cast<LONG64>(size));
    MemoryBarrier();
    InterlockedExchange(&result.pending, 0);
}

/** @param url Candidate request URL. @return True only for the exact local route. */
[[nodiscard]] bool is_local_url(const char* url) noexcept {
    if (url == nullptr) {
        return false;
    }
    const std::size_t length = strnlen_s(url, kUrlInspectionCapacity);
    return length == kLocalUrl.size() && std::string_view(url, length) == kLocalUrl;
}

/**
 * Encodes a borrowed State view into the request's response buffer.
 * @param context EncodeContext supplied by the GET replacement.
 * @param view Read-only package rows and the stable public GUID.
 * @return True when the whole body fits.
 */
[[nodiscard]] bool encode_view(void* context, const state::content_manifest::View& view) noexcept {
    auto& encode = *static_cast<EncodeContext*>(context);
    return middleware::content::manifest::encode(
        state::entitlements::get(), view.rows, view.guid, encode.output, encode.size);
}

/** Exact ABI of the fetch wrapper this replacement stands in for. */
using EnqueueGet = std::int64_t(__fastcall*)(
    void*, const char*, void*, std::int64_t, std::uint64_t, char, char) noexcept;

/** Cap on how far we read an unmapped URL. Only its length is logged, never its text. */
constexpr std::size_t kUnmappedUrlInspectionCapacity = 512;

/** @param url Requested URL, reported only by length. */
void log_unmapped(const char* url) noexcept {
    const std::size_t length = url != nullptr ? strnlen_s(url, kUnmappedUrlInspectionCapacity) : 0;
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=content_config stage=get route=unmapped length=%zu "
                                      "result=ok",
                                      length);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @param size Encoded manifest size in bytes, as reported to the Client. */
void log_manifest_served(std::size_t size) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=content_config stage=get route=manifest bytes=%zu "
                                      "result=ok",
                                      size);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Finishes the local ContentConfig GET here. It makes no outbound call.
 * @param resultValue Client-owned async result prefix.
 * @param url Null-terminated request URL.
 * @param responseBody Caller-owned response storage.
 * @param responseCapacity Size of that storage in bytes.
 * @param transaction Unused. Kept for the hook ABI.
 * @param flagA Unused. Kept for the hook ABI.
 * @param useTls Unused. Kept for the hook ABI.
 * @return 1 for every local completion, or -1 when the result prefix is null.
 */
std::int64_t enqueue_get_impl(const coordinator::CallLease& lease,
                              void* resultValue,
                              const char* url,
                              void* responseBody,
                              std::int64_t responseCapacity,
                              std::uint64_t transaction,
                              char flagA,
                              char useTls) noexcept {
    if (core::settings::get().client.externalServer.enabled) {
        // The URL comes from the config getter, which answers the external route in this mode.
        const auto call = reinterpret_cast<EnqueueGet>(lease.original);
        if (call == nullptr) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=content_config stage=external result=fail reason=original");
            return kMissingResult;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=content_config stage=external result=ok");
        return call(resultValue, url, responseBody, responseCapacity, transaction, flagA, useTls);
    }
    (void)transaction;
    (void)flagA;
    (void)useTls;
    if (resultValue == nullptr) {
        return kMissingResult;
    }

    auto& result = *static_cast<HttpRequestResult*>(resultValue);
    begin_request(result);
    if (!lease.accepting || !is_local_url(url) || responseCapacity < 0
        || (responseCapacity != 0 && responseBody == nullptr)) {
        // An unmapped route finishes here with an empty success. It never goes outbound.
        log_unmapped(url);
        complete_request(result, kHttpOk, 0);
        return kCompletedResult;
    }

    EncodeContext context{
        std::span(static_cast<std::byte*>(responseBody),
                  static_cast<std::size_t>(responseCapacity)),
    };
    // The encoder writes into the response span, so the size cannot pass the buffer size.
    if (!state::content_manifest::visit_snapshot(&encode_view, &context)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=content_config stage=get route=manifest result=encode_fail");
        complete_request(result, kHttpUnavailable, 0);
        return kCompletedResult;
    }
    log_manifest_served(context.size);
    complete_request(result, kHttpOk, context.size);
    return kCompletedResult;
}

/** Exact protected replacement body for the local ContentConfig GET route. */
__declspec(noinline) std::int64_t __fastcall enqueue_get_body(void* resultValue,
                                                              const char* url,
                                                              void* responseBody,
                                                              std::int64_t responseCapacity,
                                                              std::uint64_t transaction,
                                                              char flagA,
                                                              char useTls) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::enqueueGet);
    const std::int64_t result = enqueue_get_impl(
        lease, resultValue, url, responseBody, responseCapacity, transaction, flagA, useTls);
    coordinator::g_callEgress(lease.tracked);
    return result;
}

} // namespace

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_get_body);
}

} // namespace sunrise::client::hooks::network::content_config::request
