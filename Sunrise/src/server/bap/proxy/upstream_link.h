#pragma once

#include <WinSock2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../client/network/consumer.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../state/runtime/state.h"

namespace sunrise::server::bap::proxy::upstream_link {

enum class LinkStage : std::uint8_t {
    idle,
    connecting,
    helloSent,

    ready,
    failed,
};

struct PendingForward {
    std::uint32_t downstreamConnectionId{};
    std::uint32_t taskId{};
    /** The full response tuple must match before it can complete this request. */
    std::uint16_t expectedResponseService{};
    bool inUse{};

    bool plaintextForward{};
    std::uint64_t queuedTick{};
};

/** Concurrent correlations; additional requests stay in the bounded held-input queue. */
inline constexpr std::size_t kPendingCapacity = 4;

inline constexpr std::size_t kLinkFrameCapacity = client::network::kBapFrameCapacity;
inline constexpr std::uint64_t kRetryIntervalMs = 2000;
inline constexpr std::uint64_t kConnectTimeoutMs = 5000;
inline constexpr std::uint64_t kHelloTimeoutMs = 5000;

inline constexpr std::uint64_t kKeepaliveIntervalMs = 5000;
inline constexpr std::uint64_t kResponseTimeoutMs = 30'000;
inline constexpr std::uint32_t kOriginatedTaskIdBase = 0x80000000U;

struct UpstreamLink {
    std::uint32_t downstreamConnectionId{};
    SOCKET socket{INVALID_SOCKET};
    LinkStage stage{LinkStage::idle};
    bool established{};
    std::uint64_t nextAttemptTick{};
    std::uint64_t attemptStartedTick{};
    std::uint64_t helloSentTick{};
    std::uint64_t lastActivityTick{};
    std::uint32_t helloTaskId{};
    /**
     * Correlation counter for requests THIS link originates (pass-through forwards reuse the
     * downstream client's own taskId instead and never touch this). Starts at
     * `kOriginatedTaskIdBase`; queue admission still refuses any live correlation collision.
     */
    std::uint32_t nextOriginatedTaskId{kOriginatedTaskIdBase};
    std::array<std::byte, state::kAesKeySize> sessionKey{};
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::array<std::byte, state::kBapNonceSize> receiveNonce{};
    std::array<std::byte, kLinkFrameCapacity> stream{};
    std::size_t streamSize{};
    /** A refused callback leaves the frame, receive nonce and correlation unchanged. */
    bool receiveBlocked{};
    std::uint64_t blockedSince{};
    std::array<std::byte, kLinkFrameCapacity * kPendingCapacity> output{};
    std::size_t outputOffset{};
    std::size_t outputSize{};
    std::array<PendingForward, kPendingCapacity> pending{};
};

void reset(UpstreamLink& link) noexcept;
/** Retry only before the first completed hello; established native requests cannot be replayed. */
[[nodiscard]] bool retry_initial(UpstreamLink& link, std::uint64_t now) noexcept;

/**
 * Services one nonblocking connection. Callbacks return false for temporary output pressure;
 * they must then leave application state unchanged. The same frame is offered again when
 * serviced. Response deadlines exclude time spent waiting for this local consumer.
 */
void service_link(UpstreamLink& link,
                  std::uint64_t now,
                  bool (*onNotification)(UpstreamLink& link,
                                         std::span<const std::byte> plaintextPayload,
                                         bool plaintextFrame),
                  bool (*onResponse)(UpstreamLink& link,
                                     const PendingForward& forward,
                                     std::span<const std::byte> plaintextPayload)) noexcept;

[[nodiscard]] bool queue_forward(UpstreamLink& link,
                                 std::uint32_t downstreamConnectionId,
                                 std::uint16_t service,
                                 std::uint32_t taskId,
                                 std::uint16_t expectedResponseService,
                                 std::span<const std::byte> body) noexcept;

[[nodiscard]] bool queue_forward_plaintext(UpstreamLink& link,
                                           std::uint32_t downstreamConnectionId,
                                           std::uint16_t service,
                                           std::uint32_t taskId,
                                           std::uint16_t expectedResponseService,
                                           std::span<const std::byte> body) noexcept;

[[nodiscard]] bool send_fire_and_forget(UpstreamLink& link,
                                        std::uint16_t service,
                                        std::uint32_t taskId,
                                        std::span<const std::byte> body) noexcept;

void close_link(UpstreamLink& link,
                const char* reason,
                void (*onAbandoned)(UpstreamLink& link, const PendingForward& forward)) noexcept;

} // namespace sunrise::server::bap::proxy::upstream_link
