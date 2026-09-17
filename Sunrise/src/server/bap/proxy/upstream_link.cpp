#include "upstream_link.h"

#include <Windows.h>

#include <WS2tcpip.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/encoding/byte_order.h"
#include "../../../state/account/account_context.h"
#include "../../../state/account/account_token.h"
#include "../../../state/account/shared_channel_material.h"
#include "../../../state/runtime/runtime.h"
#include "../../../state/social/social_feed.h"

namespace sunrise::server::bap::proxy::upstream_link {
namespace {
using middleware::bap::FrameType;
using middleware::bap::RequestService;
using middleware::bap::ResponseService;
constexpr std::byte kReceiveDirectionMask{1};

void report(const UpstreamLink& link, const char* stage, const char* reason) noexcept {
    std::array<char, 160> line{};
    const int size = std::snprintf(line.data(),
                                   line.size(),
                                   "ev=proxy conn=%u stage=%s reason=%s",
                                   link.downstreamConnectionId,
                                   stage,
                                   reason);
    if (size > 0) {
        core::log::write(
            core::log::Channel::server,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(size), line.size() - 1)});
    }
}

void close_socket(UpstreamLink& link) noexcept {
    if (link.socket != INVALID_SOCKET) {
        closesocket(link.socket);
    }
    link.socket = INVALID_SOCKET;
    link.streamSize = link.outputOffset = link.outputSize = 0;
}

void fail(UpstreamLink& link, const char* reason) noexcept {
    report(link, "failed", reason);
    close_socket(link);
    link.stage = LinkStage::failed;
}

// A partial nonblocking send keeps the exact suffix. No later frame may overtake it.
bool flush_output(UpstreamLink& link) noexcept {
    if (link.outputSize == 0) {
        return true;
    }
    const int sent = send(link.socket,
                          reinterpret_cast<const char*>(link.output.data() + link.outputOffset),
                          static_cast<int>(link.outputSize - link.outputOffset),
                          0);
    if (sent > 0) {
        link.outputOffset += static_cast<std::size_t>(sent);
        if (link.outputOffset == link.outputSize) {
            link.outputOffset = link.outputSize = 0;
        }
        return true;
    }
    if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        return true;
    }
    fail(link, "send");
    return false;
}

bool enqueue_frame(UpstreamLink& link, std::span<const std::byte> frame) noexcept {
    if (frame.size() > kLinkFrameCapacity || !flush_output(link)) {
        return false;
    }
    const auto pending = link.outputSize - link.outputOffset;
    if (frame.size() > link.output.size() - pending) {
        return false;
    }
    if (link.outputOffset != 0) {
        std::memmove(link.output.data(), link.output.data() + link.outputOffset, pending);
    }
    std::copy(
        frame.begin(), frame.end(), link.output.begin() + static_cast<std::ptrdiff_t>(pending));
    link.outputOffset = 0;
    link.outputSize = pending + frame.size();
    return true;
}

bool send_hello(UpstreamLink& link) noexcept {
    std::array<std::byte, 36> body{};
    body[0] = std::byte{0x0A};
    body[1] = std::byte{0x20};
    state::account::signon_token(state::account_primary_soid(state::kLocalAccount),
                                 std::span(body).subspan(2, 32));
    body[34] = std::byte{0x10};
    body[35] = std::byte{1};
    std::array<std::byte, 64> payload{}, framed{};
    std::size_t payloadSize{}, framedSize{};
    link.helloTaskId = link.nextOriginatedTaskId++;
    const bool encoded =
        middleware::bap::encode_request_payload(
            RequestService::serverHello, link.helloTaskId, body, payload, payloadSize)
        && middleware::bap::encode_frame(
            FrameType::plaintext0, std::span(payload).first(payloadSize), framed, framedSize)
        && enqueue_frame(link, std::span(framed).first(framedSize));
    SecureZeroMemory(body.data(), body.size());
    return encoded;
}

bool begin_connect(UpstreamLink& link, std::uint64_t now) noexcept {
    const auto& upstream = core::settings::get().server.upstream;
    link.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (link.socket == INVALID_SOCKET) {
        fail(link, "socket");
        return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(link.socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        fail(link, "nonblocking");
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(upstream.bapPort);
    if (inet_pton(AF_INET, upstream.host.data(), &address.sin_addr) != 1) {
        fail(link, "address");
        return false;
    }
    const int result =
        connect(link.socket, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            fail(link, "connect");
            return false;
        }
    }
    link.stage = LinkStage::connecting;
    link.attemptStartedTick = now;
    return true;
}

bool notification_service(std::uint16_t service) noexcept {
    using middleware::bap::NotificationService;
    return service == static_cast<std::uint16_t>(NotificationService::activityMessage)
           || service == static_cast<std::uint16_t>(NotificationService::natPunchIntro)
           || service == static_cast<std::uint16_t>(NotificationService::queuezUpdate)
           || service == static_cast<std::uint16_t>(NotificationService::requestRelayConnection);
}

/** Services the shim originates for itself. They never reach native notification forwarding. */
bool internal_service(std::uint16_t service) noexcept {
    return service == state::social::feed::kPublicationNotice;
}

bool inbound(UpstreamLink& link,
             std::span<const std::byte> plaintext,
             bool plaintextFrame,
             bool (*onNotification)(UpstreamLink&, std::span<const std::byte>, bool),
             bool (*onResponse)(UpstreamLink&, const PendingForward&, std::span<const std::byte>),
             bool (*onInternal)(UpstreamLink&, std::span<const std::byte>)) noexcept {
    if (plaintext.size() < 2) {
        fail(link, "short_payload");
        return false;
    }
    const auto service = middleware::encoding::read_u16_be(plaintext.first<2>());
    if (internal_service(service)) {
        return !onInternal || onInternal(link, plaintext);
    }
    if (notification_service(service)) {
        return !onNotification || onNotification(link, plaintext, plaintextFrame);
    }
    middleware::bap::ResponseFrame response{};
    if (!middleware::bap::parse_response_payload(plaintext, response)) {
        fail(link, "response_shape");
        return false;
    }
    for (auto& forward : link.pending) {
        if (!forward.inUse || forward.taskId != response.taskId) {
            continue;
        }
        if (forward.expectedResponseService != response.serviceId) {
            fail(link, "response_service");
            return false;
        }
        const auto completed = forward;
        if (onResponse && !onResponse(link, completed, plaintext)) {
            return false;
        }
        forward = {};
        return true;
    }
    fail(link, "unexpected_response");
    return false;
}

bool receive_hello(UpstreamLink& link,
                   const middleware::bap::OuterFrame& outer,
                   std::uint64_t now) noexcept {
    middleware::bap::ResponseFrame response{};
    state::BapState material{};
    if ((outer.frameType != FrameType::plaintext0 && outer.frameType != FrameType::plaintext2)
        || !middleware::bap::parse_response_payload(outer.payload, response)
        || response.serviceId != static_cast<std::uint16_t>(ResponseService::serverHello)
        || response.taskId != link.helloTaskId || response.status != 200
        || !middleware::secure_channel::decode_server_hello(
            state::account::shared_channel_material(), response.body, material)) {
        fail(link, "hello");
        return false;
    }
    link.sessionKey = material.sessionKey;
    link.sendNonce = material.nonce;
    link.sendNonce.back() ^= kReceiveDirectionMask;
    link.receiveNonce = material.nonce;
    SecureZeroMemory(&material, sizeof material);
    link.stage = LinkStage::ready;
    link.established = true;
    link.lastActivityTick = now;
    report(link, "ready", "hello_verified");
    return true;
}

bool send_request(UpstreamLink& link,
                  std::uint16_t service,
                  std::uint32_t taskId,
                  std::span<const std::byte> body,
                  bool plaintext) noexcept {
    static std::array<std::byte, kLinkFrameCapacity> payload{}, sealed{}, framed{};
    std::size_t payloadSize{}, sealedSize{}, frameSize{};
    if (!middleware::bap::encode_request_payload(
            static_cast<RequestService>(service), taskId, body, payload, payloadSize)) {
        return false;
    }
    auto wirePayload = std::span<const std::byte>(payload).first(payloadSize);
    if (!plaintext) {
        if (!middleware::secure_channel::seal_frame(
                link.sessionKey, link.sendNonce, wirePayload, sealed, sealedSize)) {
            return false;
        }
        wirePayload = std::span(sealed).first(sealedSize);
    }
    const bool queued =
        middleware::bap::encode_frame(plaintext ? FrameType::plaintext0 : FrameType::encrypted,
                                      wirePayload,
                                      framed,
                                      frameSize)
        && enqueue_frame(link, std::span(framed).first(frameSize));
    SecureZeroMemory(payload.data(), payloadSize);
    if (queued && !plaintext) {
        middleware::secure_channel::advance_nonce(link.sendNonce);
    }
    return queued;
}

bool queue(UpstreamLink& link,
           std::uint32_t connectionId,
           std::uint16_t service,
           std::uint32_t taskId,
           std::uint16_t expectedResponse,
           std::span<const std::byte> body,
           bool plaintext) noexcept {
    if (link.stage != LinkStage::ready || link.receiveBlocked) {
        return false;
    }
    PendingForward* free = nullptr;
    for (auto& slot : link.pending) {
        if (slot.inUse && slot.taskId == taskId) {
            return false;
        }
        if (!slot.inUse && !free) {
            free = &slot;
        }
    }
    if (!free || !send_request(link, service, taskId, body, plaintext)) {
        return false;
    }
    const auto now = GetTickCount64();
    *free = PendingForward{connectionId, taskId, expectedResponse, true, plaintext, now};
    link.lastActivityTick = now;
    return true;
}
} // namespace

void reset(UpstreamLink& link) noexcept {
    close_socket(link);
    link.stage = LinkStage::idle;
    link.established = false;
    link.receiveBlocked = false;
    link.blockedSince = 0;
    link.nextAttemptTick = link.attemptStartedTick = link.helloSentTick = link.lastActivityTick = 0;
    link.helloTaskId = 0;
    link.nextOriginatedTaskId = kOriginatedTaskIdBase;
    SecureZeroMemory(link.sessionKey.data(), link.sessionKey.size());
    SecureZeroMemory(link.sendNonce.data(), link.sendNonce.size());
    SecureZeroMemory(link.receiveNonce.data(), link.receiveNonce.size());
    for (auto& slot : link.pending) {
        slot = {};
    }
}

bool retry_initial(UpstreamLink& link, std::uint64_t now) noexcept {
    if (link.stage != LinkStage::failed || link.established) {
        return false;
    }
    reset(link);
    link.nextAttemptTick = now + kRetryIntervalMs;
    report(link, "retry", "initial_connection");
    return true;
}

void service_link(UpstreamLink& link,
                  std::uint64_t now,
                  bool (*onNotification)(UpstreamLink&, std::span<const std::byte>, bool),
                  bool (*onResponse)(UpstreamLink&,
                                     const PendingForward&,
                                     std::span<const std::byte>),
                  bool (*onInternal)(UpstreamLink&, std::span<const std::byte>)) noexcept {
    if (link.downstreamConnectionId == 0 || link.stage == LinkStage::failed) {
        return;
    }
    if (link.stage == LinkStage::idle
        && (now < link.nextAttemptTick || !begin_connect(link, now))) {
        return;
    }
    fd_set read{}, write{}, except{};
    FD_SET(link.socket, &read);
    FD_SET(link.socket, &except);
    if (link.stage == LinkStage::connecting || link.outputSize != 0) {
        FD_SET(link.socket, &write);
    }
    timeval timeout{};
    if (select(0, &read, &write, &except, &timeout) == SOCKET_ERROR
        || FD_ISSET(link.socket, &except)) {
        fail(link, "select");
        return;
    }
    if (link.stage == LinkStage::connecting) {
        if (!FD_ISSET(link.socket, &write)) {
            if (now - link.attemptStartedTick >= kConnectTimeoutMs) {
                fail(link, "connect_timeout");
            }
            return;
        }
        int error{}, length = sizeof error;
        if (getsockopt(link.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length)
                == SOCKET_ERROR
            || error != 0 || !send_hello(link)) {
            fail(link, "connect_hello");
            return;
        }
        link.stage = LinkStage::helloSent;
        link.helloSentTick = now;
    }
    if (FD_ISSET(link.socket, &write) && !flush_output(link)) {
        return;
    }
    if (link.stage == LinkStage::helloSent && now - link.helloSentTick >= kHelloTimeoutMs) {
        fail(link, "hello_timeout");
        return;
    }
    bool mayRead = FD_ISSET(link.socket, &read) != 0;
    for (;;) {
        middleware::bap::OuterFrame outer{};
        std::size_t consumed{};
        const auto result = middleware::bap::parse_stream_frame(
            std::span(link.stream).first(link.streamSize), link.stream.size(), outer, consumed);
        if (result == middleware::bap::StreamFrameResult::incomplete) {
            if (!mayRead) {
                break;
            }
            mayRead = false;
            const auto free = link.stream.size() - link.streamSize;
            if (free == 0) {
                fail(link, "stream_full");
                return;
            }
            const int received = recv(link.socket,
                                      reinterpret_cast<char*>(link.stream.data() + link.streamSize),
                                      static_cast<int>(free),
                                      0);
            if (received == 0) {
                fail(link, "eof");
                return;
            }
            if (received < 0) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    fail(link, "receive");
                }
                return;
            }
            link.streamSize += static_cast<std::size_t>(received);
            link.lastActivityTick = now;
            continue;
        }
        if (result == middleware::bap::StreamFrameResult::invalid) {
            fail(link, "framing");
            return;
        }
        bool accepted = true;
        if (link.stage == LinkStage::helloSent) {
            if (!receive_hello(link, outer, now)) {
                return;
            }
        } else if (outer.frameType == FrameType::encrypted) {
            static std::array<std::byte, kLinkFrameCapacity> plaintext{};
            std::size_t size{};
            if (!middleware::secure_channel::open_frame(
                    link.sessionKey, link.receiveNonce, outer.payload, plaintext, size)) {
                fail(link, "authentication");
                return;
            }
            accepted = inbound(link,
                               std::span(plaintext).first(size),
                               false,
                               onNotification,
                               onResponse,
                               onInternal);
            SecureZeroMemory(plaintext.data(), size);
        } else {
            accepted = inbound(link, outer.payload, true, onNotification, onResponse, onInternal);
        }
        if (link.stage != LinkStage::ready) {
            return;
        }
        if (!accepted) {
            if (!link.receiveBlocked) {
                link.receiveBlocked = true;
                link.blockedSince = now;
            }
            return;
        }
        if (link.receiveBlocked) {
            const auto paused = now - link.blockedSince;
            for (auto& pending : link.pending) {
                if (pending.inUse) {
                    pending.queuedTick += paused;
                }
            }
            link.receiveBlocked = false;
            link.blockedSince = 0;
        }
        if (outer.frameType == FrameType::encrypted) {
            middleware::secure_channel::advance_nonce(link.receiveNonce);
        }
        std::memmove(link.stream.data(), link.stream.data() + consumed, link.streamSize - consumed);
        link.streamSize -= consumed;
    }
    for (const auto& slot : link.pending) {
        if (slot.inUse && now - slot.queuedTick >= kResponseTimeoutMs) {
            fail(link, "response_timeout");
            return;
        }
    }
    if (link.stage == LinkStage::ready && now - link.lastActivityTick >= kKeepaliveIntervalMs) {
        const bool echoPending =
            std::any_of(link.pending.begin(), link.pending.end(), [](const PendingForward& slot) {
                return slot.inUse
                       && slot.expectedResponseService
                              == static_cast<std::uint16_t>(ResponseService::echo);
            });
        if (!echoPending) {
            if (queue_forward(link,
                              0,
                              static_cast<std::uint16_t>(RequestService::echo),
                              link.nextOriginatedTaskId,
                              static_cast<std::uint16_t>(ResponseService::echo),
                              {})) {
                ++link.nextOriginatedTaskId;
            }
        }
    }
}

bool queue_forward(UpstreamLink& link,
                   std::uint32_t connectionId,
                   std::uint16_t service,
                   std::uint32_t taskId,
                   std::uint16_t expectedResponse,
                   std::span<const std::byte> body) noexcept {
    return queue(link, connectionId, service, taskId, expectedResponse, body, false);
}
bool queue_forward_plaintext(UpstreamLink& link,
                             std::uint32_t connectionId,
                             std::uint16_t service,
                             std::uint32_t taskId,
                             std::uint16_t expectedResponse,
                             std::span<const std::byte> body) noexcept {
    return queue(link, connectionId, service, taskId, expectedResponse, body, true);
}
bool send_fire_and_forget(UpstreamLink& link,
                          std::uint16_t service,
                          std::uint32_t taskId,
                          std::span<const std::byte> body) noexcept {
    if (link.stage != LinkStage::ready || link.receiveBlocked
        || !send_request(link, service, taskId, body, false)) {
        return false;
    }
    link.lastActivityTick = GetTickCount64();
    return true;
}
void close_link(UpstreamLink& link,
                const char* reason,
                void (*onAbandoned)(UpstreamLink&, const PendingForward&)) noexcept {
    for (auto& slot : link.pending) {
        if (!slot.inUse) {
            continue;
        }
        const auto abandoned = slot;
        slot = {};
        if (onAbandoned) {
            onAbandoned(link, abandoned);
        }
    }
    report(link, "closed", reason);
    reset(link);
}
} // namespace sunrise::server::bap::proxy::upstream_link
