#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../client/network/consumer.h"
#include "../../../state/runtime/state.h"
#include "proxy_routing.h"

namespace sunrise::server::bap::proxy {

inline constexpr std::size_t kReplyEntryCapacity = client::network::kBapFrameCapacity;

void open_link(std::uint32_t downstreamConnectionId) noexcept;

void close_link(std::uint32_t downstreamConnectionId) noexcept;

void service(std::uint64_t now) noexcept;
[[nodiscard]] bool failed(std::uint32_t downstreamConnectionId) noexcept;

[[nodiscard]] bool upstream_ready(std::uint32_t downstreamConnectionId) noexcept;

[[nodiscard]] bool forward_request(std::uint32_t downstreamConnectionId,
                                   std::uint16_t service,
                                   std::uint16_t expectedResponseService,
                                   std::uint32_t taskId,
                                   std::span<const std::byte> body,
                                   std::array<std::byte, state::kBapNonceSize>& sendNonce) noexcept;

[[nodiscard]] bool forward_uncorrelated(std::uint32_t downstreamConnectionId,
                                        std::uint16_t service,
                                        std::uint32_t taskId,
                                        std::span<const std::byte> body) noexcept;

[[nodiscard]] bool forward_plaintext_request(std::uint32_t downstreamConnectionId,
                                             std::uint16_t service,
                                             std::uint16_t expectedResponseService,
                                             std::uint32_t taskId,
                                             std::span<const std::byte> body) noexcept;

[[nodiscard]] bool has_outstanding(std::uint32_t downstreamConnectionId) noexcept;

/**
 * Admission check on the single BAP service thread. A full ordered queue must leave inbound
 * requests buffered and deferred publications owed; neither may consume state or a send nonce.
 * framedSize zero checks only slot capacity, before a request has been decoded/composed.
 */
[[nodiscard]] bool can_enqueue_local_reply(std::uint32_t downstreamConnectionId,
                                           std::size_t framedSize = 0) noexcept;

[[nodiscard]] bool enqueue_local_reply(std::uint32_t downstreamConnectionId,
                                       std::span<const std::byte> framedBytes) noexcept;

[[nodiscard]] std::size_t drain_ordered_replies(std::uint32_t downstreamConnectionId,
                                                std::span<const std::byte, state::kAesKeySize> key,
                                                std::span<std::byte> output) noexcept;

[[nodiscard]] bool send_upstream_request(std::uint32_t downstreamConnectionId,
                                         std::uint16_t service,
                                         std::uint16_t expectedResponseService,
                                         std::span<const std::byte> body,
                                         std::uint32_t& outTaskId) noexcept;

[[nodiscard]] std::uint32_t first_ready_upstream() noexcept;

} // namespace sunrise::server::bap::proxy
