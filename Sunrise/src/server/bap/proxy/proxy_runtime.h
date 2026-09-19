#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../client/network/consumer.h"
#include "../../../state/runtime/state.h"
#include "proxy_routing.h"

namespace sunrise::server::bap::proxy {
// Every declaration below runs on the BAP service thread, serialized by the BAP session lock.

/** Sized to one full downstream frame, so a held or completed reply never spans two slots. */
inline constexpr std::size_t kReplyEntryCapacity = client::network::kBapFrameCapacity;

/** Allocates bounded input/output storage before admitting traffic; failed() reports refusal. */
void open_link(std::uint32_t downstreamConnectionId) noexcept;

/**
 * Releases the upstream link and its queued state for this connection. Safe when already
 * closed.
 */
void close_link(std::uint32_t downstreamConnectionId) noexcept;

/**
 * Drives the proxy for one tick. On the host role only the local profile publisher and social
 * push run; otherwise every open upstream link is pumped, a link that fails without a retry is
 * closed, and held forwards are flushed once their link is ready.
 */
void service(std::uint64_t now) noexcept;
/** True once this connection's upstream link has been marked failed. */
[[nodiscard]] bool failed(std::uint32_t downstreamConnectionId) noexcept;

/** True once the upstream handshake has reached the ready stage and the link has not failed. */
[[nodiscard]] bool upstream_ready(std::uint32_t downstreamConnectionId) noexcept;

/** Defers input behind unanswered native requests or full queues, before decryption or mutation. */
[[nodiscard]] bool can_accept_request(std::uint32_t downstreamConnectionId) noexcept;

/** Holds the request exactly once. No downstream response nonce is reserved until completion. */
[[nodiscard]] bool forward_request(std::uint32_t downstreamConnectionId,
                                   std::uint16_t service,
                                   std::uint16_t expectedResponseService,
                                   std::uint32_t taskId,
                                   std::span<const std::byte> body) noexcept;

/**
 * As forward_request, but expects no correlated reply: no response service and no live-task
 * duplicate check.
 */
[[nodiscard]] bool forward_uncorrelated(std::uint32_t downstreamConnectionId,
                                        std::uint16_t service,
                                        std::uint32_t taskId,
                                        std::span<const std::byte> body) noexcept;

/** As forward_request, but forwarded unencrypted rather than through the secure channel. */
[[nodiscard]] bool forward_plaintext_request(std::uint32_t downstreamConnectionId,
                                             std::uint16_t service,
                                             std::uint16_t expectedResponseService,
                                             std::uint32_t taskId,
                                             std::span<const std::byte> body) noexcept;

/** True while this connection still has completed replies queued for delivery. */
[[nodiscard]] bool has_outstanding(std::uint32_t downstreamConnectionId) noexcept;

/**
 * A full completed-output queue leaves inbound requests buffered and deferred publications
 * owed; neither may consume state or a send nonce. framedSize zero checks only slot capacity,
 * before a request has been decoded/composed.
 */
[[nodiscard]] bool can_enqueue_local_reply(std::uint32_t downstreamConnectionId,
                                           std::size_t framedSize = 0) noexcept;

/**
 * Queues an already-framed local reply behind any completed upstream replies, preserving
 * nonce order.
 */
[[nodiscard]] bool enqueue_local_reply(std::uint32_t downstreamConnectionId,
                                       std::span<const std::byte> framedBytes) noexcept;

/** Drains completed frames in nonce order; pending requests never block notifications. */
[[nodiscard]] std::size_t drain_ordered_replies(std::uint32_t downstreamConnectionId,
                                                std::span<const std::byte, state::kAesKeySize> key,
                                                std::span<std::byte> output) noexcept;

/** Assigns a task ID only after admission; refusal clears outTaskId without spending an ID. */
[[nodiscard]] bool send_upstream_request(std::uint32_t downstreamConnectionId,
                                         std::uint16_t service,
                                         std::uint16_t expectedResponseService,
                                         std::span<const std::byte> body,
                                         std::uint32_t& outTaskId) noexcept;

/** The first connection id whose upstream link is ready, or zero when none is. */
[[nodiscard]] std::uint32_t first_ready_upstream() noexcept;

} // namespace sunrise::server::bap::proxy
