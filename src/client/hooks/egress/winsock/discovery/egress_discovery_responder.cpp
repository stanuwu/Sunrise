#include "egress_discovery_responder.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "../../../../../core/settings/settings.h"

namespace sunrise::client::hooks::egress::winsock::discovery {
namespace {

/** Demonware discovery uses these two fixed destination ports. */
constexpr std::uint16_t kFirstDiscoveryPort = 3074;
constexpr std::uint16_t kSecondDiscoveryPort = 3075;
/** A NatProbe request is two big-endian 16-bit fields. */
constexpr std::size_t kNatProbeRequestSize = 4;
/** The decoder needs the whole 128-bit NatProbe reply. */
constexpr std::size_t kNatProbeReplySize = 16;
/** The verified IP-discovery request is one type byte and a native-order version. */
constexpr std::size_t kIpDiscoveryRequestSize = 3;
/** Version 2 carries one address in 9 bytes. */
constexpr std::size_t kIpDiscoveryReplySize = 9;
/** NatProbe requests use this fixed big-endian magic value. */
constexpr std::uint16_t kNatProbeMagic = 1;
/** The verified NatProbe state machine sends request indices 1 and 2. */
constexpr std::uint16_t kFirstNatProbeIndex = 1;
constexpr std::uint16_t kSecondNatProbeIndex = 2;
/** IP-discovery reply types fill this inclusive range. */
constexpr unsigned kFirstIpDiscoveryType = 30;
constexpr unsigned kLastIpDiscoveryType = 39;
/** Version 2 picks the single-address IP-discovery reply. */
constexpr std::uint16_t kIpDiscoveryReplyVersion = 2;
/** NatProbe masks protect the echoed address and port fields. */
constexpr std::uint32_t kNatProbeAddressMask = 0x76C3F6BC;
constexpr std::uint16_t kNatProbePortMask = 0xF6BC;

enum class RequestKind {
    none,
    natProbe,
    ipDiscovery,
};

/** @return One big-endian 16-bit value from a verified request offset. */
[[nodiscard]] std::uint16_t read_big_u16(std::span<const std::byte> input,
                                         std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset]) << 8U)
           | static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset + 1]));
}

/** Writes one big-endian 16-bit value into fixed reply storage. */
void write_big_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 8U);
    output[offset + 1] = static_cast<std::byte>(value);
}

/** Writes one big-endian 32-bit value into fixed reply storage. */
void write_big_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 24U);
    output[offset + 1] = static_cast<std::byte>(value >> 16U);
    output[offset + 2] = static_cast<std::byte>(value >> 8U);
    output[offset + 3] = static_cast<std::byte>(value);
}

/** Writes one little-endian 16-bit value into fixed reply storage. */
void write_little_u16(std::span<std::byte> output,
                      std::size_t offset,
                      std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value);
    output[offset + 1] = static_cast<std::byte>(value >> 8U);
}

/** @return The verified discovery request kind, or none for unrelated bytes. */
[[nodiscard]] RequestKind classify(std::span<const std::byte> payload) noexcept {
    if (payload.size() == kNatProbeRequestSize && read_big_u16(payload, 0) == kNatProbeMagic) {
        const std::uint16_t index = read_big_u16(payload, 2);
        if (index == kFirstNatProbeIndex || index == kSecondNatProbeIndex) {
            return RequestKind::natProbe;
        }
    }
    if (payload.size() == kIpDiscoveryRequestSize) {
        const unsigned type = std::to_integer<unsigned>(payload.front());
        if (type >= kFirstIpDiscoveryType && type <= kLastIpDiscoveryType) {
            return RequestKind::ipDiscovery;
        }
    }
    return RequestKind::none;
}

/** @return True when the destination is one supported IPv4 discovery port. */
[[nodiscard]] bool make_loopback_destination(const sockaddr* destination,
                                             int destinationLength,
                                             sockaddr_in& loopback) noexcept {
    if (destination == nullptr || destinationLength < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }
    std::memcpy(&loopback, destination, sizeof(loopback));
    const std::uint16_t port = ntohs(loopback.sin_port);
    if (loopback.sin_family != AF_INET
        || (port != kFirstDiscoveryPort && port != kSecondDiscoveryPort)) {
        return false;
    }
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return true;
}

/** Closes temporary state and restores the error returned to the game. */
[[nodiscard]] Result close_with_error(SOCKET temporary, int error) noexcept {
    (void)::closesocket(temporary);
    WSASetLastError(error);
    return Result{true, SOCKET_ERROR};
}

/** @return A handled access-denied result when no retained send entry exists. */
[[nodiscard]] Result missing_send_result() noexcept {
    WSASetLastError(WSAEACCES);
    return Result{true, SOCKET_ERROR};
}

/** Normalizes Winsock's auto-bound wildcard address to its loopback route. */
void normalize_client_address(sockaddr_in& client) noexcept {
    if (client.sin_addr.s_addr == htonl(INADDR_ANY)) {
        client.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
}

/** Builds one exact verified reply and returns its byte count. */
[[nodiscard]] std::size_t build_reply(RequestKind kind,
                                      std::span<const std::byte> request,
                                      const sockaddr_in& client,
                                      std::span<std::byte> reply) noexcept {
    const std::uint32_t clientIp = ntohl(client.sin_addr.s_addr);
    const std::uint16_t clientPort = ntohs(client.sin_port);
    if (kind == RequestKind::natProbe) {
        reply[0] = request[0];
        reply[1] = request[1];
        reply[2] = request[2];
        reply[3] = request[3];
        write_big_u32(reply, 4, clientIp ^ kNatProbeAddressMask);
        write_big_u16(reply, 8, clientPort ^ kNatProbePortMask);
        return kNatProbeReplySize;
    }
    reply[0] = request[0];
    write_little_u16(reply, 1, kIpDiscoveryReplyVersion);
    write_big_u32(reply, 3, clientIp);
    write_little_u16(reply, 7, clientPort);
    return kIpDiscoveryReplySize;
}

} // namespace

/** @return Whether the request was handled and its replacement send result. */
Result handle(SOCKET socket,
              std::span<const std::byte> payload,
              int flags,
              const sockaddr* destination,
              int destinationLength,
              SendTo sendTo) noexcept {
    // An external server runs its own bdNet listener, so the probe must reach it unanswered.
    if (core::settings::get().client.externalServer.enabled) {
        return {};
    }
    const RequestKind kind = classify(payload);
    sockaddr_in loopback{};
    if (kind == RequestKind::none
        || !make_loopback_destination(destination, destinationLength, loopback)) {
        return {};
    }
    if (sendTo == nullptr) {
        return missing_send_result();
    }

    const SOCKET temporary = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (temporary == INVALID_SOCKET) {
        return Result{true, SOCKET_ERROR};
    }
    if (::bind(temporary,
               reinterpret_cast<const sockaddr*>(&loopback),
               static_cast<int>(sizeof(loopback)))
        == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        (void)::closesocket(temporary);
        if (error == WSAEADDRINUSE) {
            const int result = sendTo(socket,
                                      reinterpret_cast<const char*>(payload.data()),
                                      static_cast<int>(payload.size()),
                                      flags,
                                      reinterpret_cast<const sockaddr*>(&loopback),
                                      static_cast<int>(sizeof(loopback)));
            return Result{true, result};
        }
        WSASetLastError(error);
        return Result{true, SOCKET_ERROR};
    }

    const int requestResult = sendTo(socket,
                                     reinterpret_cast<const char*>(payload.data()),
                                     static_cast<int>(payload.size()),
                                     flags,
                                     reinterpret_cast<const sockaddr*>(&loopback),
                                     static_cast<int>(sizeof(loopback)));
    if (requestResult == SOCKET_ERROR) {
        return close_with_error(temporary, WSAGetLastError());
    }

    sockaddr_in client{};
    int clientLength = static_cast<int>(sizeof(client));
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&client), &clientLength)
        == SOCKET_ERROR) {
        return close_with_error(temporary, WSAGetLastError());
    }
    if (clientLength != static_cast<int>(sizeof(client)) || client.sin_family != AF_INET
        || client.sin_port == 0) {
        return close_with_error(temporary, WSAEINVAL);
    }
    normalize_client_address(client);

    std::array<std::byte, kNatProbeReplySize> reply{};
    const std::size_t replySize = build_reply(kind, payload, client, reply);
    const int replyResult = sendTo(temporary,
                                   reinterpret_cast<const char*>(reply.data()),
                                   static_cast<int>(replySize),
                                   0,
                                   reinterpret_cast<const sockaddr*>(&client),
                                   static_cast<int>(sizeof(client)));
    if (replyResult == SOCKET_ERROR) {
        return close_with_error(temporary, WSAGetLastError());
    }
    (void)::closesocket(temporary);
    return Result{true, requestResult};
}

} // namespace sunrise::client::hooks::egress::winsock::discovery
