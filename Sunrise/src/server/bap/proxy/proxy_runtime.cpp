#include <Windows.h>

#include <algorithm>
#include <memory>
#include <new>

#include "../../../core/settings/settings.h"
#include "../../../state/social/social_feed.h"
#include "../bap_session_nonce.h"
#include "../encrypted/social_feed_route.h"
#include "proxy_internal.h"
#include "proxy_profile_publisher.h"
#include "proxy_social_feed.h"
#include "upstream_link.h"

namespace sunrise::server::bap::proxy {
namespace {
using upstream_link::LinkStage;
using upstream_link::PendingForward;
using upstream_link::UpstreamLink;
constexpr auto kConnectionCount = client::network::kBapConnectionCount;
constexpr std::uint64_t kHoldTimeoutMs = 15'000;

struct HeldForward {
    std::uint16_t service{};
    std::uint16_t responseService{};
    std::uint32_t taskId{};
    std::unique_ptr<std::byte[]> body;
    std::size_t bodySize{};
    std::uint64_t queuedTick{};
    bool plaintext{};
    bool uncorrelated{};
};
struct HeldQueue {
    std::array<HeldForward, kReplyQueueCapacity> entries;
    std::size_t head{};
    std::size_t count{};
};
std::array<std::unique_ptr<UpstreamLink>, kConnectionCount> g_links;
std::array<HeldQueue, kConnectionCount> g_held;
std::array<bool, kConnectionCount> g_failed{};
bool g_profileReady{};

UpstreamLink* link_for(std::uint32_t id) noexcept {
    return id != 0 && id <= g_links.size() ? g_links[id - 1].get() : nullptr;
}

void reset_projection() noexcept {
    g_profileReady = false;
    profile_publisher::reset();
}

void abandoned(UpstreamLink& link, const PendingForward& forward) noexcept {
    if (forward.downstreamConnectionId == 0) {
        profile_publisher::abandon(
            link.downstreamConnectionId, forward.taskId, forward.expectedResponseService);
        social_feed::abandon(link.downstreamConnectionId, forward.taskId);
    }
}

void notification(UpstreamLink& link, std::span<const std::byte> payload, bool plaintext) noexcept {
    const auto id = link.downstreamConnectionId;
    if (failed(id)) {
        return;
    }
    auto* queue = queue_for(id);
    auto* nonce = plaintext ? nullptr : downstream_send_nonce(id);
    if (!queue || payload.size() > kReplyEntryCapacity || (!plaintext && !nonce)) {
        fail_connection(id, "notification_shape");
        return;
    }
    auto* entry = push_entry(*queue);
    if (!entry) {
        fail_connection(id, "notification_queue_full");
        return;
    }
    entry->needsSeal = !plaintext;
    entry->needsPlaintextFrame = plaintext;
    if (nonce) {
        entry->reservedNonce = *nonce;
        entry->hasReservedNonce = true;
        middleware::secure_channel::advance_nonce(*nonce);
    }
    std::copy(payload.begin(), payload.end(), entry->payload->begin());
    entry->payloadSize = payload.size();
    entry->ready = true;
}

void response(UpstreamLink& link,
              const PendingForward& forward,
              std::span<const std::byte> payload) noexcept {
    if (failed(link.downstreamConnectionId)) {
        return;
    }
    middleware::bap::ResponseFrame parsed{};
    if (!middleware::bap::parse_response_payload(payload, parsed)
        || parsed.serviceId != forward.expectedResponseService || parsed.taskId != forward.taskId) {
        fail_connection(link.downstreamConnectionId, "response_tuple");
        return;
    }
    if (forward.downstreamConnectionId == 0) {
        if (parsed.serviceId == state::social::feed::kFeedResponse) {
            if (parsed.status != 200
                || !social_feed::acknowledge(
                    link.downstreamConnectionId, forward.taskId, parsed.body)) {
                fail_connection(link.downstreamConnectionId, "social_response");
            }
            return;
        }
        if (parsed.status != 200) {
            profile_publisher::abandon(
                link.downstreamConnectionId, forward.taskId, parsed.serviceId);
            return;
        }
        if (profile_publisher::acknowledge(
                link.downstreamConnectionId, forward.taskId, parsed.serviceId)) {
            g_profileReady = true;
        }
        return;
    }
    auto* queue = queue_for(forward.downstreamConnectionId);
    auto* entry = queue ? find_placeholder(*queue, forward.taskId) : nullptr;
    if (!entry || payload.size() > kReplyEntryCapacity) {
        fail_connection(link.downstreamConnectionId, "response_queue");
        return;
    }
    std::copy(payload.begin(), payload.end(), entry->payload->begin());
    entry->payloadSize = payload.size();
    entry->needsSeal = !forward.plaintextForward;
    entry->needsPlaintextFrame = forward.plaintextForward;
    entry->ready = true;
}

bool hold(std::uint32_t id,
          std::uint16_t service,
          std::uint16_t responseService,
          std::uint32_t taskId,
          std::span<const std::byte> body,
          bool plaintext,
          bool uncorrelated) noexcept {
    auto* link = link_for(id);
    if (!link || link->downstreamConnectionId != id || failed(id)) {
        return false;
    }
    auto& queue = g_held[id - 1];
    if (queue.count == queue.entries.size()
        || body.size() > upstream_link::kLinkFrameCapacity - 22) {
        return false;
    }
    std::unique_ptr<std::byte[]> copy;
    if (!body.empty()) {
        copy.reset(new (std::nothrow) std::byte[body.size()]);
        if (!copy) {
            return false;
        }
        std::copy(body.begin(), body.end(), copy.get());
    }
    auto& slot = queue.entries[(queue.head + queue.count) % queue.entries.size()];
    slot = HeldForward{service,
                       responseService,
                       taskId,
                       std::move(copy),
                       body.size(),
                       GetTickCount64(),
                       plaintext,
                       uncorrelated};
    ++queue.count;
    return true;
}

void flush_held(std::uint32_t id, std::uint64_t now) noexcept {
    auto& queue = g_held[id - 1];
    auto& link = *g_links[id - 1];
    while (queue.count != 0 && !failed(id)) {
        auto& slot = queue.entries[queue.head];
        if (now - slot.queuedTick >= kHoldTimeoutMs) {
            fail_connection(id, "forward_timeout");
            return;
        }
        if (!g_profileReady || link.stage != LinkStage::ready) {
            return;
        }
        const std::span<const std::byte> body(slot.body.get(), slot.bodySize);
        const bool queued =
            slot.uncorrelated
                ? upstream_link::send_fire_and_forget(link, slot.service, slot.taskId, body)
                : (slot.plaintext
                       ? upstream_link::queue_forward_plaintext(
                             link, id, slot.service, slot.taskId, slot.responseService, body)
                       : upstream_link::queue_forward(
                             link, id, slot.service, slot.taskId, slot.responseService, body));
        if (!queued) {
            if (link.stage == LinkStage::failed) {
                fail_connection(id, "forward_send");
            }
            return;
        }
        slot = {};
        queue.head = (queue.head + 1) % queue.entries.size();
        --queue.count;
    }
}
} // namespace

bool failed(std::uint32_t id) noexcept {
    return id != 0 && id <= g_failed.size() && g_failed[id - 1];
}

void fail_connection(std::uint32_t id, const char* reason) noexcept {
    auto* link = link_for(id);
    if (!link || failed(id)) {
        return;
    }
    g_failed[id - 1] = true;
    report(id, "connection", "failed", 0, 0, reason);
    upstream_link::close_link(*link, reason, abandoned);
    if (first_ready_upstream() == 0) {
        reset_projection();
        social_feed::reset();
    }
    // The transport observes failed() through BapResponse after the BAP lock is released.
}

void open_link(std::uint32_t id) noexcept {
    if (!core::settings::get().server.upstream.enabled) {
        return;
    }
    if (id == 0 || id > g_links.size()) {
        return;
    }
    close_link(id);
    g_links[id - 1].reset(new (std::nothrow) UpstreamLink{});
    auto* link = link_for(id);
    if (!link) {
        g_failed[id - 1] = true;
        return;
    }
    upstream_link::reset(*link);
    link->downstreamConnectionId = id;
    g_failed[id - 1] = false;
    g_held[id - 1] = {};
    reset_queue(id);
}

void close_link(std::uint32_t id) noexcept {
    auto* link = link_for(id);
    if (!link) {
        return;
    }
    if (link->downstreamConnectionId != 0) {
        upstream_link::close_link(*link, "downstream_closed", abandoned);
    }
    link->downstreamConnectionId = 0;
    g_failed[id - 1] = false;
    g_held[id - 1] = {};
    reset_queue(id);
    g_links[id - 1].reset();
    if (first_ready_upstream() == 0) {
        reset_projection();
        social_feed::reset();
    }
}

void service(std::uint64_t now) noexcept {
    if (core::settings::role() == core::settings::Role::host) {
        profile_publisher::service(now);
        encrypted::service_host_social(now);
        return;
    }
    if (!core::settings::get().server.upstream.enabled) {
        return;
    }
    for (auto& owned : g_links) {
        if (!owned) {
            continue;
        }
        auto& link = *owned;
        const auto id = link.downstreamConnectionId;
        if (id == 0 || failed(id)) {
            continue;
        }
        upstream_link::service_link(link, now, notification, response);
        if (link.stage == LinkStage::failed && !upstream_link::retry_initial(link, now)) {
            fail_connection(id, "upstream_failed");
        }
        // Another channel for this account does not invalidate its acknowledged public profile.
    }
    profile_publisher::service(now);
    for (const auto& link : g_links) {
        if (link && link->downstreamConnectionId != 0) {
            flush_held(link->downstreamConnectionId, now);
        }
    }
    if (g_profileReady) {
        social_feed::service(now);
    }
}

bool upstream_ready(std::uint32_t id) noexcept {
    const auto* link = link_for(id);
    return link && link->downstreamConnectionId == id && link->stage == LinkStage::ready
           && !failed(id);
}

bool forward_request(std::uint32_t id,
                     std::uint16_t service,
                     std::uint16_t responseService,
                     std::uint32_t taskId,
                     std::span<const std::byte> body,
                     std::array<std::byte, state::kBapNonceSize>& nonce) noexcept {
    auto* queue = queue_for(id);
    auto* entry = queue ? push_entry(*queue) : nullptr;
    if (!entry) {
        return false;
    }
    if (!hold(id, service, responseService, taskId, body, false, false)) {
        pop_tail(*queue);
        return false;
    }
    entry->taskId = taskId;
    entry->hasReservedNonce = true;
    entry->reservedNonce = nonce;
    middleware::secure_channel::advance_nonce(nonce);
    return true;
}

bool forward_uncorrelated(std::uint32_t id,
                          std::uint16_t service,
                          std::uint32_t taskId,
                          std::span<const std::byte> body) noexcept {
    return hold(id, service, 0, taskId, body, false, true);
}

bool forward_plaintext_request(std::uint32_t id,
                               std::uint16_t service,
                               std::uint16_t responseService,
                               std::uint32_t taskId,
                               std::span<const std::byte> body) noexcept {
    auto* queue = queue_for(id);
    auto* entry = queue ? push_entry(*queue) : nullptr;
    if (!entry) {
        return false;
    }
    if (!hold(id, service, responseService, taskId, body, true, false)) {
        pop_tail(*queue);
        return false;
    }
    entry->taskId = taskId;
    return true;
}

bool send_upstream_request(std::uint32_t id,
                           std::uint16_t service,
                           std::uint16_t responseService,
                           std::span<const std::byte> body,
                           std::uint32_t& taskId) noexcept {
    auto* link = link_for(id);
    if (!upstream_ready(id)) {
        return false;
    }
    taskId = link->nextOriginatedTaskId++;
    return upstream_link::queue_forward(*link, 0, service, taskId, responseService, body);
}

std::uint32_t first_ready_upstream() noexcept {
    for (const auto& link : g_links) {
        if (link && upstream_ready(link->downstreamConnectionId)) {
            return link->downstreamConnectionId;
        }
    }
    return 0;
}
} // namespace sunrise::server::bap::proxy
