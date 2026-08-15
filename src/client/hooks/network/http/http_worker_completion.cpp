#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "internal.h"

namespace sunrise::client::hooks::network::http {
namespace {

/** Request state 2 makes the worker report a finished HTTP result. */
constexpr LONG kRequestComplete = 2;
/** Returning 5 makes the worker skip the native external operation. */
constexpr std::int64_t kSkipNativeOperation = 5;
/** HTTP 503 finishes a blocked request without contacting a server. */
constexpr std::uint32_t kHttpUnavailable = 503;

/** ABI offset of the result pending flag. */
constexpr std::size_t kResultPendingOffset = 8;
/** ABI offset of the result request-state field. */
constexpr std::size_t kResultStateOffset = 12;
/** ABI offset of the result HTTP-status field. */
constexpr std::size_t kResultStatusOffset = 16;
/** Native result prefixes are 24 bytes with 64-bit alignment. */
constexpr std::size_t kResultSize = 24;

/** Native result fields the outer worker writes after the executor returns. */
struct HttpRequestResult final {
    volatile LONG64 transferred{};
    volatile LONG pending{};
    LONG requestState{};
    LONG httpStatus{};
};

static_assert(offsetof(HttpRequestResult, pending) == kResultPendingOffset);
static_assert(offsetof(HttpRequestResult, requestState) == kResultStateOffset);
static_assert(offsetof(HttpRequestResult, httpStatus) == kResultStatusOffset);
static_assert(sizeof(HttpRequestResult) == kResultSize);

/** Per-thread fallback, so a null result pointer is still safe to write to. */
thread_local HttpRequestResult g_discardResult{};

/** ABI offset of the wrapper request-state field. */
constexpr std::size_t kRequestStateOffset = 64;
/** ABI offset of the wrapper transfer counters. */
constexpr std::size_t kTransferCountersOffset = 88;
/** ABI offset of the wrapper transfer-rate field. */
constexpr std::size_t kTransferRateOffset = 152;
/** ABI offset of the wrapper HTTP-status field. */
constexpr std::size_t kHttpStatusOffset = 1216;

/** Uploaded and downloaded byte counts reset together for each local request. */
struct TransferCounters final {
    std::uint32_t uploaded{};
    std::uint32_t downloaded{};
};

/** Native wrapper fields the outer worker reads after the executor returns. */
struct HttpWrapper final {
    std::array<std::byte, kRequestStateOffset> statePrefix{};
    volatile LONG requestState{};
    std::array<std::byte, kTransferCountersOffset - kRequestStateOffset - sizeof(requestState)>
        countersPrefix{};
    TransferCounters counters{};
    std::array<std::byte, kTransferRateOffset - kTransferCountersOffset - sizeof(TransferCounters)>
        ratePrefix{};
    float transferRate{};
    std::array<std::byte, kHttpStatusOffset - kTransferRateOffset - sizeof(transferRate)>
        statusPrefix{};
    std::uint32_t httpStatus{};
};

static_assert(offsetof(HttpWrapper, requestState) == kRequestStateOffset);
static_assert(offsetof(HttpWrapper, counters) == kTransferCountersOffset);
static_assert(offsetof(HttpWrapper, counters.downloaded) == kTransferCountersOffset + 4);
static_assert(offsetof(HttpWrapper, transferRate) == kTransferRateOffset);
static_assert(offsetof(HttpWrapper, httpStatus) == kHttpStatusOffset);

} // namespace

/** @return Per-thread storage that stands in for a null worker result pointer. */
void* discard_result() noexcept {
    return &g_discardResult;
}

/** @param wrapper Game-owned wrapper. @return The value that marks the request handled. */
std::int64_t block_request(void* wrapper) noexcept {
    publish_completion(wrapper, 0, kHttpUnavailable);
    return kSkipNativeOperation;
}

/** Sets the fields the outer HTTP worker copies into the result prefix. */
void publish_completion(void* wrapperValue,
                        std::uint32_t responseSize,
                        std::uint32_t httpStatus) noexcept {
    auto& wrapper = *static_cast<HttpWrapper*>(wrapperValue);
    wrapper.counters = {};
    wrapper.transferRate = 0.0F;
    wrapper.counters.downloaded = responseSize;
    wrapper.httpStatus = httpStatus;
    (void)InterlockedExchange(&wrapper.requestState, kRequestComplete);
}

} // namespace sunrise::client::hooks::network::http
