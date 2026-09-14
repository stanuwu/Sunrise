#pragma once

#include <memory>

#include "proxy_runtime.h"

namespace sunrise::server::bap::proxy {

// One BAP service thread owns these queues. Callers keep the existing connection lock
// discipline.
struct ReplyEntry {
    bool inUse{};
    bool ready{};
    bool needsSeal{};

    bool needsPlaintextFrame{};
    std::uint32_t taskId{};

    std::array<std::byte, state::kBapNonceSize> reservedNonce{};
    bool hasReservedNonce{};
    std::unique_ptr<std::array<std::byte, kReplyEntryCapacity>> payload;
    std::size_t payloadSize{};
};

inline constexpr std::size_t kReplyQueueCapacity = 8;

struct ReplyQueue {
    std::array<ReplyEntry, kReplyQueueCapacity> entries{};
    std::uint8_t head{};
    std::uint8_t count{};
};

[[nodiscard]] ReplyQueue* queue_for(std::uint32_t connectionId) noexcept;
void reset_queue(std::uint32_t connectionId) noexcept;
void fail_connection(std::uint32_t connectionId, const char* reason) noexcept;

[[nodiscard]] ReplyEntry* push_entry(ReplyQueue& queue) noexcept;

void pop_tail(ReplyQueue& queue) noexcept;

void pop_head(ReplyQueue& queue) noexcept;

[[nodiscard]] ReplyEntry* find_placeholder(ReplyQueue& queue, std::uint32_t taskId) noexcept;

void report(std::uint32_t connectionId,
            const char* stage,
            const char* result,
            std::uint16_t service,
            std::uint32_t taskId,
            const char* reason) noexcept;

} // namespace sunrise::server::bap::proxy
