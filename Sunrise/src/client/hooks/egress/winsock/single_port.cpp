#include "single_port.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/gameplay/nat/single_port_frame.h"
#include "../internal.h"
#include "../policy/policy.h"

namespace sunrise::client::hooks::egress::single_port {
namespace {
namespace wire = middleware::gameplay::single_port;
sockaddr_in carrier() noexcept {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    const auto address = policy::redirect_octets();
    std::memcpy(&endpoint.sin_addr, address.data(), address.size());
    const auto& settings = core::settings::get();
    endpoint.sin_port = htons(core::settings::hosts_session() ? settings.server.bapPort
                                                              : settings.server.upstream.bapPort);
    return endpoint;
}
int fail(int error) noexcept {
    WSASetLastError(error);
    return SOCKET_ERROR;
}
} // namespace
int unsupported() noexcept {
    // Reports of an unsupported call per run. Enough to show the caller once without a repeat
    // from every later call filling the log; the refusal itself is unconditional.
    constexpr unsigned kMaxReports = 8;
    static std::atomic<unsigned> reports{};
    if (reports.fetch_add(1) < kMaxReports) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=single_port result=unsupported_udp_api");
    }
    return fail(WSAEOPNOTSUPP);
}
int send(SOCKET socket,
         const char* data,
         int size,
         int flags,
         const sockaddr* destination,
         int destinationSize) noexcept {
    const auto call = original<decltype(&::sendto)>(HookSlot::sendTo);
    if (!call) {
        return fail(WSAEACCES);
    }
    if (size < 0 || (!data && size)) {
        return fail(WSAEFAULT);
    }
    if (size > static_cast<int>(wire::kPayload)) {
        return fail(WSAEMSGSIZE);
    }
    if (flags) {
        return unsupported();
    }
    sockaddr_in logical{};
    if (!policy::redirect_ipv4(destination, destinationSize, logical, socket)) {
        return fail(WSAEACCES);
    }
    std::array<std::byte, wire::kCapacity> packet{};
    const auto length =
        wire::encode({wire::Kind::request,
                      ntohl(logical.sin_addr.s_addr),
                      ntohs(logical.sin_port),
                      {reinterpret_cast<const std::byte*>(data), static_cast<std::size_t>(size)}},
                     packet);
    if (!length) {
        return fail(WSAEINVAL);
    }
    const auto target = carrier();
    const int sent = call(socket,
                          reinterpret_cast<const char*>(packet.data()),
                          static_cast<int>(length),
                          0,
                          reinterpret_cast<const sockaddr*>(&target),
                          sizeof target);
    return sent == SOCKET_ERROR ? SOCKET_ERROR : size;
}
int receive(
    SOCKET socket, char* data, int size, int flags, sockaddr* source, int* sourceSize) noexcept {
    const auto call = original<decltype(&::recvfrom)>(HookSlot::recvFrom);
    if (!call) {
        return fail(WSAEACCES);
    }
    if (size < 0 || (!data && size)
        || (source && (!sourceSize || *sourceSize < static_cast<int>(sizeof(sockaddr_in))))) {
        return fail(WSAEFAULT);
    }
    if (flags & ~MSG_PEEK) {
        return unsupported();
    }
    const auto hub = carrier();
    // Drain a bounded number of foreign/malformed packets, including when the caller peeks.
    // Past the bound the call reports that nothing is readable, so the caller simply asks again
    // on its next pass; nothing buffered is lost.
    constexpr unsigned kDrainAttempts = 8;
    for (unsigned attempt = 0; attempt < kDrainAttempts; ++attempt) {
        std::array<std::byte, wire::kCapacity> packet{};
        sockaddr_in sender{};
        int senderSize = sizeof sender;
        const auto count = call(socket,
                                reinterpret_cast<char*>(packet.data()),
                                static_cast<int>(packet.size()),
                                flags,
                                reinterpret_cast<sockaddr*>(&sender),
                                &senderSize);
        if (count == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }
        wire::Frame frame{};
        if (sender.sin_addr.s_addr != hub.sin_addr.s_addr || sender.sin_port != hub.sin_port
            || !wire::decode(std::span(packet).first(static_cast<std::size_t>(count)), frame)
            || frame.kind != wire::Kind::delivery) {
            if (flags & MSG_PEEK) {
                (void)call(socket,
                           reinterpret_cast<char*>(packet.data()),
                           static_cast<int>(packet.size()),
                           0,
                           nullptr,
                           nullptr);
            }
            continue;
        }
        if (source) {
            sockaddr_in logical{};
            logical.sin_family = AF_INET;
            logical.sin_addr.s_addr = htonl(frame.address);
            logical.sin_port = htons(frame.port);
            std::memcpy(source, &logical, sizeof logical);
            *sourceSize = sizeof logical;
        }
        const auto copied = (std::min)(static_cast<std::size_t>(size), frame.payload.size());
        if (copied) {
            std::memcpy(data, frame.payload.data(), copied);
        }
        if (copied != frame.payload.size()) {
            return fail(WSAEMSGSIZE);
        }
        return static_cast<int>(copied);
    }
    return fail(WSAEWOULDBLOCK);
}
} // namespace sunrise::client::hooks::egress::single_port
