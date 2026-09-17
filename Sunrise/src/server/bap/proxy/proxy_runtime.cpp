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
// Both the outer and request headers occupy 6 bytes; encrypted requests also carry a GCM tag.
constexpr std::size_t kHeldBodyCapacity = upstream_link::kLinkFrameCapacity - 12;

struct HeldForward {
    std::uint16_t service{};
    std::uint16_t responseService{};
    std::uint32_t taskId{};
    std::unique_ptr<std::byte[]> body;
    std::size_t bodySize{};
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

bool notification(UpstreamLink& link, std::span<const std::byte> payload, bool plaintext) noexcept {
    const auto id = link.downstreamConnectionId;
    if (failed(id)) {
        return false;
    }
    auto* queue = queue_for(id);
    auto* nonce = plaintext ? nullptr : downstream_send_nonce(id);
    if (!queue || payload.size() > kReplyEntryCapacity || (!plaintext && !nonce)) {
        fail_connection(id, "notification_shape");
        return false;
    }
    auto* entry = push_entry(*queue);
    if (!entry) {
        return false;
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
    return true;
}

bool response(UpstreamLink& link,
              const PendingForward& forward,
              std::span<const std::byte> payload) noexcept {
    if (failed(link.downstreamConnectionId)) {
        return false;
    }
    middleware::bap::ResponseFrame parsed{};
    if (!middleware::bap::parse_response_payload(payload, parsed)
        || parsed.serviceId != forward.expectedResponseService || parsed.taskId != forward.taskId) {
        fail_connection(link.downstreamConnectionId, "response_tuple");
        return false;
    }
    if (forward.downstreamConnectionId == 0) {
        if (parsed.serviceId == state::social::feed::kFeedResponse) {
            if (parsed.status != 200
                || !social_feed::acknowledge(
                    link.downstreamConnectionId, forward.taskId, parsed.body)) {
                fail_connection(link.downstreamConnectionId, "social_response");
            }
            return !failed(link.downstreamConnectionId);
        }
        if (parsed.status != 200) {
            profile_publisher::abandon(
                link.downstreamConnectionId, forward.taskId, parsed.serviceId);
            return true;
        }
        if (profile_publisher::acknowledge(
                link.downstreamConnectionId, forward.taskId, parsed.serviceId)) {
            g_profileReady = true;
        }
        return true;
    }
    return notification(link, payload, forward.plaintextForward);
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
    const auto capacity =
        kHeldBodyCapacity - (plaintext ? 0 : middleware::secure_channel::kFrameTagSize);
    if (queue.count == queue.entries.size() || body.size() > capacity) {
        return false;
    }
    if (!uncorrelated) {
        // A duplicate live task is invalid, not capacity pressure: retaining it would stall
        // the held queue behind a correlation that cannot be sent.
        for (const auto& pending : link->pending) {
            if (pending.inUse && pending.taskId == taskId) {
                return false;
            }
        }
        for (std::size_t i = 0; i < queue.count; ++i) {
            const auto& held = queue.entries[(queue.head + i) % queue.entries.size()];
            if (!held.uncorrelated && held.taskId == taskId) {
                return false;
            }
        }
    }
    auto& slot = queue.entries[(queue.head + queue.count) % queue.entries.size()];
    if (!slot.body) {
        return false;
    }
    std::copy(body.begin(), body.end(), slot.body.get());
    slot.service = service;
    slot.responseService = responseService;
    slot.taskId = taskId;
    slot.bodySize = body.size();
    slot.plaintext = plaintext;
    slot.uncorrelated = uncorrelated;
    ++queue.count;
    return true;
}

void flush_held(std::uint32_t id) noexcept {
    auto& queue = g_held[id - 1];
    auto& link = *g_links[id - 1];
    while (queue.count != 0 && !failed(id)) {
        auto& slot = queue.entries[queue.head];
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
        SecureZeroMemory(slot.body.get(), slot.bodySize);
        slot.bodySize = 0;
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
    // Each open proxy connection owns eight reply buffers, eight held bodies and the link's
    // five wire buffers (~10.3 MiB). Allocate here so request admission cannot fail mid-mutation.
    if (!prepare_queue(id)) {
        close_link(id);
        g_failed[id - 1] = true;
        return;
    }
    for (auto& held : g_held[id - 1].entries) {
        held.body.reset(new (std::nothrow) std::byte[kHeldBodyCapacity]);
        if (!held.body) {
            close_link(id);
            g_failed[id - 1] = true;
            return;
        }
    }
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
            flush_held(link->downstreamConnectionId);
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

bool can_accept_request(std::uint32_t id) noexcept {
    const auto* link = link_for(id);
    if (!link || failed(id) || !can_enqueue_local_reply(id)) {
        return false;
    }
    const auto& held = g_held[id - 1];
    if (held.count == held.entries.size()) {
        return false;
    }
    // Native replies match only the head of the client's pending-request ring. Leave later
    // input in the transport until an earlier forwarded reply reaches the output queue;
    // otherwise a local reply can overtake it. Notifications and shim-originated requests
    // still progress through service() while downstream input is deferred.
    for (std::size_t i = 0; i < held.count; ++i) {
        if (!held.entries[(held.head + i) % held.entries.size()].uncorrelated) {
            return false;
        }
    }
    return std::none_of(link->pending.begin(), link->pending.end(), [](const auto& pending) {
        return pending.inUse && pending.downstreamConnectionId != 0;
    });
}

bool forward_request(std::uint32_t id,
                     std::uint16_t service,
                     std::uint16_t responseService,
                     std::uint32_t taskId,
                     std::span<const std::byte> body) noexcept {
    return hold(id, service, responseService, taskId, body, false, false);
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
    return hold(id, service, responseService, taskId, body, true, false);
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
