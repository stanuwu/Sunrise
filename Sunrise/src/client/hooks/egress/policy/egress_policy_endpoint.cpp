#include <array>
#include <cstring>

#include "../../../../core/network_service_socket.h"
#include "../../../../core/settings/settings.h"
#include "../../../../state/network/peer_routes.h"
#include "../../../../state/runtime/runtime.h"
#include "egress_policy_logging.h"

namespace sunrise::client::hooks::egress::policy {
namespace {

/** Access denied marks every outbound socket call blocked by policy. */
constexpr int kBlockedSocketError = WSAEACCES;
/** The policy accepts one exact IPv4 address, not a range. */
using core::settings::kLoopbackOctets;

bool datagram(SOCKET socket) noexcept {
    int type{};
    int size = sizeof type;
    return socket != INVALID_SOCKET
           && ::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0
           && type == SOCK_DGRAM;
}
bool peer_target(const sockaddr_in& endpoint, bool udp) noexcept {
    return udp && core::settings::multiplayer()
           && state::network::peer_routes::allows(
               {ntohl(endpoint.sin_addr.s_addr), ntohs(endpoint.sin_port)});
}
bool local_account_peer(const sockaddr_in& endpoint, SOCKET socket, bool udp) noexcept {
    if (udp || !core::settings::multiplayer()
        || std::memcmp(&endpoint.sin_addr, kLoopbackOctets.data(), kLoopbackOctets.size()) != 0) {
        return false;
    }
    const auto port = state::sign_on().relayPort;
    if (port && ntohs(endpoint.sin_port) == port) {
        return true;
    }
    sockaddr_in local{};
    int length = sizeof local;
    return port && socket != INVALID_SOCKET
           && ::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0
           && length >= static_cast<int>(sizeof local) && local.sin_family == AF_INET
           && ntohs(local.sin_port) == port
           && std::memcmp(&local.sin_addr, kLoopbackOctets.data(), kLoopbackOctets.size()) == 0;
}

} // namespace

/** Reads the single address every redirected socket operation may reach. */
std::array<unsigned char, 4> redirect_octets() noexcept {
    if (!core::settings::get().multiplayerEnabled) {
        return kLoopbackOctets;
    }
    if (core::settings::role() == core::settings::Role::host) {
        return core::settings::get().server.gameplay.transportAddress;
    }
    if (core::settings::get().server.upstream.enabled) {
        return core::settings::get().server.upstream.address;
    }
    const core::settings::client::external::Settings& external =
        core::settings::get().client.externalServer;
    if (!external.enabled) {
        return kLoopbackOctets;
    }
    return {external.address[0], external.address[1], external.address[2], external.address[3]};
}

/** Checks one caller-owned socket address without a name lookup. */
bool is_redirect_target(const sockaddr* address, int addressLength, SOCKET socket) noexcept {
    if (address == nullptr || addressLength < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }

    sockaddr_in endpoint{};
    std::memcpy(&endpoint, address, sizeof(endpoint));
    if (endpoint.sin_family != AF_INET) {
        return false;
    }
    if (core::settings::get().multiplayerEnabled
        && core::network::ServiceSocketScope::owns(socket)) {
        return true;
    }
    const bool udp = datagram(socket);
    if (peer_target(endpoint, udp)) {
        return true;
    }
    if (local_account_peer(endpoint, socket, udp)) {
        return true;
    }
    const std::array<unsigned char, 4> octets = redirect_octets();
    return endpoint.sin_family == AF_INET
           && std::memcmp(&endpoint.sin_addr, octets.data(), octets.size()) == 0;
}

/** Copies one valid IPv4 endpoint and replaces only its address bytes. */
bool redirect_ipv4(const sockaddr* address,
                   int addressLength,
                   sockaddr_in& redirected,
                   SOCKET socket) noexcept {
    if (address == nullptr || addressLength < static_cast<int>(sizeof(redirected))) {
        return false;
    }

    std::memcpy(&redirected, address, sizeof(redirected));
    if (redirected.sin_family != AF_INET) {
        return false;
    }
    if (core::settings::get().multiplayerEnabled
        && core::network::ServiceSocketScope::owns(socket)) {
        return true;
    }
    const bool udp = datagram(socket);
    if (peer_target(redirected, udp)) {
        return true;
    }
    if (local_account_peer(redirected, socket, udp)) {
        return true;
    }
    if (!udp && core::settings::multiplayer()
        && ntohs(redirected.sin_port) == state::sign_on().relayPort) {
        std::memcpy(&redirected.sin_addr, kLoopbackOctets.data(), kLoopbackOctets.size());
        return true;
    }
    const std::array<unsigned char, 4> octets = redirect_octets();
    std::memcpy(&redirected.sin_addr, octets.data(), octets.size());
    return true;
}

/** Reads the peer of one connected socket and applies the endpoint policy. */
bool has_redirect_target_peer(SOCKET socket) noexcept {
    sockaddr_storage peer{};
    int peerLength = static_cast<int>(sizeof(peer));
    if (::getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peerLength) == SOCKET_ERROR) {
        return false;
    }
    return is_redirect_target(reinterpret_cast<const sockaddr*>(&peer), peerLength, socket);
}

/** Applies the fail-closed gate and logs its decision. */
bool allow_socket_call(SocketOperation operation,
                       bool targetsRedirect,
                       bool originalAvailable) noexcept {
    const bool allowed = targetsRedirect && originalAvailable;
    log_decision(operation, targetsRedirect, allowed);
    return allowed;
}

/** @return SOCKET_ERROR after publishing the stable policy error. */
int deny_socket_call() noexcept {
    WSASetLastError(kBlockedSocketError);
    return SOCKET_ERROR;
}

} // namespace sunrise::client::hooks::egress::policy
