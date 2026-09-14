#include <algorithm>
#include <cstdio>
#include <new>

#include "../../../core/logging/log.h"
#include "../../../middleware/bap/frame.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "proxy_internal.h"

namespace sunrise::server::bap::proxy {
namespace {
std::array<ReplyQueue, client::network::kBapConnectionCount> g_queues;

void clear(ReplyEntry& entry) noexcept {
    entry.inUse = entry.ready = entry.needsSeal = entry.needsPlaintextFrame =
        entry.hasReservedNonce = false;
    entry.taskId = 0;
    entry.payloadSize = 0;
    entry.reservedNonce = {};
}
} // namespace

ReplyQueue* queue_for(std::uint32_t id) noexcept {
    return id != 0 && id <= g_queues.size() ? &g_queues[id - 1] : nullptr;
}

void reset_queue(std::uint32_t id) noexcept {
    auto* queue = queue_for(id);
    if (!queue) {
        return;
    }
    queue->head = queue->count = 0;
    for (auto& entry : queue->entries) {
        clear(entry);
        entry.payload.reset();
    }
}

ReplyEntry* push_entry(ReplyQueue& queue) noexcept {
    if (queue.count == queue.entries.size()) {
        return nullptr;
    }
    auto& entry = queue.entries[(queue.head + queue.count) % queue.entries.size()];
    if (!entry.payload) {
        entry.payload.reset(new (std::nothrow) std::array<std::byte, kReplyEntryCapacity>);
    }
    if (!entry.payload) {
        return nullptr;
    }
    clear(entry);
    entry.inUse = true;
    ++queue.count;
    return &entry;
}

void pop_tail(ReplyQueue& queue) noexcept {
    if (queue.count == 0) {
        return;
    }
    clear(queue.entries[(queue.head + queue.count - 1) % queue.entries.size()]);
    --queue.count;
}

void pop_head(ReplyQueue& queue) noexcept {
    if (queue.count == 0) {
        return;
    }
    clear(queue.entries[queue.head]);
    queue.head = static_cast<std::uint8_t>((queue.head + 1) % queue.entries.size());
    --queue.count;
}

ReplyEntry* find_placeholder(ReplyQueue& queue, std::uint32_t taskId) noexcept {
    for (std::size_t i = 0; i < queue.count; ++i) {
        auto& entry = queue.entries[(queue.head + i) % queue.entries.size()];
        if (entry.inUse && !entry.ready && entry.taskId == taskId) {
            return &entry;
        }
    }
    return nullptr;
}

void report(std::uint32_t id,
            const char* stage,
            const char* result,
            std::uint16_t service,
            std::uint32_t taskId,
            const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int size = std::snprintf(line.data(),
                                   line.size(),
                                   "ev=proxy conn=%u stage=%s svc=%u task=%u result=%s reason=%s",
                                   id,
                                   stage,
                                   static_cast<unsigned>(service),
                                   taskId,
                                   result,
                                   reason);
    if (size > 0) {
        core::log::write(
            core::log::Channel::server,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(size), line.size() - 1)});
    }
}

bool has_outstanding(std::uint32_t id) noexcept {
    const auto* queue = queue_for(id);
    return queue && queue->count != 0;
}

bool can_enqueue_local_reply(std::uint32_t id, std::size_t size) noexcept {
    const auto* queue = queue_for(id);
    return queue && !failed(id) && queue->count < queue->entries.size()
           && size <= kReplyEntryCapacity;
}

bool enqueue_local_reply(std::uint32_t id, std::span<const std::byte> bytes) noexcept {
    if (!can_enqueue_local_reply(id, bytes.size())) {
        return false;
    }
    auto* entry = push_entry(*queue_for(id));
    if (!entry) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), entry->payload->begin());
    entry->payloadSize = bytes.size();
    entry->ready = true;
    return true;
}

std::size_t drain_ordered_replies(std::uint32_t id,
                                  std::span<const std::byte, state::kAesKeySize> key,
                                  std::span<std::byte> output) noexcept {
    auto* queue = queue_for(id);
    if (!queue || failed(id)) {
        return 0;
    }
    std::size_t written{};
    static std::array<std::byte, kReplyEntryCapacity + 128> framed{}, sealed{};
    while (queue->count != 0) {
        auto& head = queue->entries[queue->head];
        if (!head.ready) {
            break;
        }
        if (head.needsSeal && !head.hasReservedNonce) {
            fail_connection(id, "reply_nonce");
            return 0;
        }
        std::size_t frameSize{}, sealedSize{};
        bool encoded{};
        const auto payload = std::span(*head.payload).first(head.payloadSize);
        if (head.needsSeal) {
            encoded = middleware::secure_channel::seal_frame(
                          key, head.reservedNonce, payload, sealed, sealedSize)
                      && middleware::bap::encode_frame(middleware::bap::FrameType::encrypted,
                                                       std::span(sealed).first(sealedSize),
                                                       framed,
                                                       frameSize);
        } else if (head.needsPlaintextFrame) {
            encoded = middleware::bap::encode_frame(
                middleware::bap::FrameType::plaintext0, payload, framed, frameSize);
        } else {
            std::copy(payload.begin(), payload.end(), framed.begin());
            frameSize = payload.size();
            encoded = true;
        }
        if (!encoded) {
            fail_connection(id, "reply_encode");
            return 0;
        }
        if (frameSize > output.size() - written) {
            break;
        }
        std::copy_n(
            framed.begin(), frameSize, output.begin() + static_cast<std::ptrdiff_t>(written));
        written += frameSize;
        pop_head(*queue);
    }
    return written;
}
} // namespace sunrise::server::bap::proxy
