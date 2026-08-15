#pragma once

#include <array>
#include <string_view>

#include "../platform/sdk.h"

namespace sunrise::client::hooks::egress::policy {

/** Socket operation categories emitted by the egress policy log. */
enum class SocketOperation : unsigned char {
    connect,
    send,
};

/** Name and provider categories emitted by the egress policy log. */
enum class NameOperation : unsigned char {
    resolve,
    reverse,
    dns,
    control,
};

/** Logs one name or provider decision with the requested name and selector. */
void log_name_decision(NameOperation operation,
                       bool allowed,
                       std::string_view name = {},
                       unsigned selector = 0,
                       unsigned status = 0,
                       const void* caller = nullptr) noexcept;

/** Logs one name decision whose requested name is wide. */
void log_name_decision(NameOperation operation,
                       bool allowed,
                       std::wstring_view name,
                       unsigned selector = 0,
                       unsigned status = 0,
                       const void* caller = nullptr) noexcept;

/** Returns the one IPv4 address every redirected operation may reach. */
[[nodiscard]] std::array<unsigned char, 4> redirect_octets() noexcept;

/** Returns true only for an endpoint that is exactly the AF_INET redirect target. */
[[nodiscard]] bool is_redirect_target(const sockaddr* address, int addressLength) noexcept;

/** Copies one IPv4 endpoint and replaces only its address with the redirect target. */
[[nodiscard]] bool
redirect_ipv4(const sockaddr* address, int addressLength, sockaddr_in& redirected) noexcept;

/** Returns true only when a connected socket's peer is the exact redirect target. */
[[nodiscard]] bool has_redirect_target_peer(SOCKET socket) noexcept;

/** Logs and returns whether a redirected call can use the original. */
[[nodiscard]] bool
allow_socket_call(SocketOperation operation, bool targetsRedirect, bool originalAvailable) noexcept;

/** Sets the stable Winsock policy error and returns SOCKET_ERROR. */
[[nodiscard]] int deny_socket_call() noexcept;

} // namespace sunrise::client::hooks::egress::policy
