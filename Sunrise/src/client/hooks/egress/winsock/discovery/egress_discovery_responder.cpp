#include "egress_discovery_responder.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/gameplay/nat/discovery.h"

namespace sunrise::client::hooks::egress::winsock::discovery {
namespace {

namespace wire = middleware::gameplay::nat::discovery;

/** @return True when the destination is one supported IPv4 discovery port. */
[[nodiscard]] bool make_loopback_destination(const sockaddr* destination,
                                             int destinationLength,
                                             sockaddr_in& loopback) noexcept {
    if (destination == nullptr || destinationLength < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }
    std::memcpy(&loopback, destination, sizeof(loopback));
    const std::uint16_t port = ntohs(loopback.sin_port);
    if (loopback.sin_family != AF_INET || (port != wire::kFirstPort && port != wire::kSecondPort)) {
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
    const auto kind = wire::classify(payload);
    sockaddr_in loopback{};
    if (kind == wire::Request::none
        || !make_loopback_destination(destination, destinationLength, loopback)) {
        return {};
    }
    if (sendTo == nullptr) {
        return missing_send_result();
    }

    if (core::settings::multiplayer()) {
        const auto& address = core::settings::role() == core::settings::Role::host
                                  ? core::settings::get().server.gameplay.transportAddress
                                  : core::settings::get().server.upstream.address;
        std::memcpy(&loopback.sin_addr, address.data(), address.size());
        // The native probe must use the game's own UDP socket so the server observes its NAT
        // mapping. Neither the client's wildcard bind nor a separate socket identifies it.
        return {true,
                sendTo(socket,
                       reinterpret_cast<const char*>(payload.data()),
                       static_cast<int>(payload.size()),
                       flags,
                       reinterpret_cast<const sockaddr*>(&loopback),
                       sizeof loopback)};
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

    std::array<std::byte, wire::kReplyCapacity> reply{};
    const std::size_t replySize =
        wire::reply(payload, ntohl(client.sin_addr.s_addr), ntohs(client.sin_port), reply);
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
