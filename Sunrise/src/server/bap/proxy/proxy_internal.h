#pragma once

#include <memory>

#include "proxy_runtime.h"

namespace sunrise::server::bap::proxy {

/** Completed output in nonce order, owned by the BAP service thread under its session lock. */
struct ReplyEntry {
    bool needsSeal{};

    bool needsPlaintextFrame{};
    std::array<std::byte, state::kBapNonceSize> reservedNonce{};
    bool hasReservedNonce{};
    std::unique_ptr<std::array<std::byte, kReplyEntryCapacity>> payload;
    std::size_t payloadSize{};
};

/**
 * Bounded burst storage; capacity pressure defers input until the socket drains.
 * The depth is chosen rather than measured, so overflow is deferral and never a dropped reply.
 */
inline constexpr std::size_t kReplyQueueCapacity = 8;

/** One connection's completed output, held in delivery order as a fixed ring buffer. */
struct ReplyQueue {
    std::array<ReplyEntry, kReplyQueueCapacity> entries{};
    std::uint8_t head{};
    std::uint8_t count{};
};

/** Borrows a fixed table slot under BAP serialization; null for an invalid connection id. */
[[nodiscard]] ReplyQueue* queue_for(std::uint32_t connectionId) noexcept;
/** Allocates all payload slots before this connection accepts requests. */
[[nodiscard]] bool prepare_queue(std::uint32_t connectionId) noexcept;
/** Clears the queue and releases its payload storage. */
void reset_queue(std::uint32_t connectionId) noexcept;
/**
 * Marks the connection failed, closes its upstream link, and logs the reason.
 * A no-op once already failed.
 */
void fail_connection(std::uint32_t connectionId, const char* reason) noexcept;

/**
 * Reserves a slot under BAP serialization; null at capacity or without payload storage.
 *
 * Borrows contents until the entry is popped or the connection is reset.
 */
[[nodiscard]] ReplyEntry* push_entry(ReplyQueue& queue) noexcept;

/** Discards the oldest entry. A no-op on an empty queue. */
void pop_head(ReplyQueue& queue) noexcept;

/** Writes one proxy event line to the server log. */
void report(std::uint32_t connectionId,
            const char* stage,
            const char* result,
            std::uint16_t service,
            std::uint32_t taskId,
            const char* reason) noexcept;

} // namespace sunrise::server::bap::proxy
