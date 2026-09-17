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

/** Bounded burst storage; capacity pressure defers input until the socket drains. */
inline constexpr std::size_t kReplyQueueCapacity = 8;

struct ReplyQueue {
    std::array<ReplyEntry, kReplyQueueCapacity> entries{};
    std::uint8_t head{};
    std::uint8_t count{};
};

[[nodiscard]] ReplyQueue* queue_for(std::uint32_t connectionId) noexcept;
/** Allocates all payload slots before this connection accepts requests. */
[[nodiscard]] bool prepare_queue(std::uint32_t connectionId) noexcept;
void reset_queue(std::uint32_t connectionId) noexcept;
void fail_connection(std::uint32_t connectionId, const char* reason) noexcept;

[[nodiscard]] ReplyEntry* push_entry(ReplyQueue& queue) noexcept;

void pop_head(ReplyQueue& queue) noexcept;

void report(std::uint32_t connectionId,
            const char* stage,
            const char* result,
            std::uint16_t service,
            std::uint32_t taskId,
            const char* reason) noexcept;

} // namespace sunrise::server::bap::proxy
