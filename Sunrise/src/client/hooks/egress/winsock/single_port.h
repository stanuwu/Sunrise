#pragma once
#include <WinSock2.h>

#include "../../../../core/network_service_socket.h"
#include "../../../../core/settings/settings.h"

namespace sunrise::client::hooks::egress::single_port {
[[nodiscard]] inline bool enabled(SOCKET socket) noexcept {
    if ((!core::settings::hosts_session() && core::settings::role() != core::settings::Role::client)
        || core::settings::get().client.externalServer.enabled
        || core::network::ServiceSocketScope::owns(socket)) {
        return false;
    }
    int type{};
    int size = sizeof type;
    return getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0
           && type == SOCK_DGRAM;
}
// The native datagram backend uses sendto/recvfrom. Other UDP APIs fail explicitly until
// their completion semantics are implemented; they must never leak raw packets to extra ports.
int unsupported() noexcept;
int send(SOCKET socket,
         const char* data,
         int size,
         int flags,
         const sockaddr* destination,
         int destinationSize) noexcept;
int receive(
    SOCKET socket, char* data, int size, int flags, sockaddr* source, int* sourceSize) noexcept;
} // namespace sunrise::client::hooks::egress::single_port
