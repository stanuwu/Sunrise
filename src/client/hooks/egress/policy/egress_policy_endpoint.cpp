#include <array>
#include <cstring>

#include "../../../../core/settings/settings.h"
#include "egress_policy_logging.h"

namespace sunrise::client::hooks::egress::policy {
namespace {

/** Access denied marks every outbound socket call blocked by policy. */
constexpr int kBlockedSocketError = WSAEACCES;
/** The policy accepts one exact IPv4 address, not a range. */
constexpr std::array<unsigned char, 4> kLoopbackOctets{127, 0, 0, 1};

} // namespace

/** Reads the single address every redirected socket operation may reach. */
std::array<unsigned char, 4> redirect_octets() noexcept {
    const core::settings::client::external::Settings& external =
        core::settings::get().client.externalServer;
    if (!external.enabled) {
        return kLoopbackOctets;
    }
    return {external.address[0], external.address[1], external.address[2], external.address[3]};
}

/** Checks one caller-owned socket address without a name lookup. */
bool is_redirect_target(const sockaddr* address, int addressLength) noexcept {
    if (address == nullptr || addressLength < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }

    sockaddr_in endpoint{};
    std::memcpy(&endpoint, address, sizeof(endpoint));
    const std::array<unsigned char, 4> octets = redirect_octets();
    return endpoint.sin_family == AF_INET
           && std::memcmp(&endpoint.sin_addr, octets.data(), octets.size()) == 0;
}

/** Copies one valid IPv4 endpoint and replaces only its address bytes. */
bool redirect_ipv4(const sockaddr* address, int addressLength, sockaddr_in& redirected) noexcept {
    if (address == nullptr || addressLength < static_cast<int>(sizeof(redirected))) {
        return false;
    }

    std::memcpy(&redirected, address, sizeof(redirected));
    if (redirected.sin_family != AF_INET) {
        return false;
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
    return is_redirect_target(reinterpret_cast<const sockaddr*>(&peer), peerLength);
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
