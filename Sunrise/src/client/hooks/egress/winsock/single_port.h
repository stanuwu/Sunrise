#pragma once
#include <WinSock2.h>

#include "../../../../core/network_service_socket.h"
#include "../../../../core/settings/settings.h"

namespace sunrise::client::hooks::egress::single_port {
/**
 * @param socket Socket the replacement was entered for.
 * @return True while `socket` is a datagram socket that must be tunnelled through the shared
 * port: multiplayer is enabled, the role is client or hosting a session, no external server
 * override is set, and no owning service scope has already claimed the socket.
 */
[[nodiscard]] inline bool enabled(SOCKET socket) noexcept {
    if (!core::settings::multiplayer()
        || (!core::settings::hosts_session()
            && core::settings::role() != core::settings::Role::client)
        || core::settings::get().client.externalServer.enabled
        || core::network::ServiceSocketScope::owns(socket)) {
        return false;
    }
    int type{};
    int size = sizeof type;
    return getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0
           && type == SOCK_DGRAM;
}
/**
 * Fails a UDP API this framing has no completion semantics for, so a call the native backend
 * does not use is refused rather than leaking a raw packet on the shared port.
 * @return SOCKET_ERROR, with WSAEOPNOTSUPP set.
 */
int unsupported() noexcept;
/**
 * Frames one datagram to the shared port; the destination travels in the payload, not in the
 * socket call. `flags` other than zero are refused through `unsupported`.
 * @return `size` on success, or SOCKET_ERROR for a bad destination, an oversized payload, or an
 * underlying send failure.
 */
int send(SOCKET socket,
         const char* data,
         int size,
         int flags,
         const sockaddr* destination,
         int destinationSize) noexcept;
/**
 * Unwraps one framed datagram from the shared port and reports its original sender.
 * @param sourceSize In: capacity of `source`. Out: bytes written to `source`.
 * @return Payload bytes copied, or SOCKET_ERROR. Foreign or malformed frames are drained rather
 * than returned; the call reports WSAEWOULDBLOCK once the drain bound is reached.
 */
int receive(
    SOCKET socket, char* data, int size, int flags, sockaddr* source, int* sourceSize) noexcept;
} // namespace sunrise::client::hooks::egress::single_port
