#include "egress_connection_replacements.h"

#include "../../internal.h"
#include "../../policy/policy.h"
#include "../../resolver/redirect.h"

namespace sunrise::client::hooks::egress::winsock::connection {
namespace {

/** WSAConnectByList could otherwise pick an endpoint off the redirect target. */
constexpr INT kAllowedAddressCount = 1;

} // namespace

/** Redirects one complete IPv4 destination to the exact redirect target. */
int WSAAPI connect_socket(SOCKET socket, const sockaddr* name, int nameLength) noexcept {
    const auto call = original<decltype(&::connect)>(HookSlot::connect);
    sockaddr_in redirected{};
    const bool targetsRedirect = policy::redirect_ipv4(name, nameLength, redirected);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        return policy::deny_socket_call();
    }
    return call(socket,
                reinterpret_cast<const sockaddr*>(&redirected),
                static_cast<int>(sizeof(redirected)));
}

/** Redirects an IPv4 connection with caller and quality-of-service data to the redirect target. */
int WSAAPI connect_socket_ex(SOCKET socket,
                             const sockaddr* name,
                             int nameLength,
                             LPWSABUF callerData,
                             LPWSABUF calleeData,
                             LPQOS socketQos,
                             LPQOS groupQos) noexcept {
    const auto call = original<decltype(&::WSAConnect)>(HookSlot::wsaConnect);
    sockaddr_in redirected{};
    const bool targetsRedirect = policy::redirect_ipv4(name, nameLength, redirected);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        return policy::deny_socket_call();
    }
    return call(socket,
                reinterpret_cast<const sockaddr*>(&redirected),
                static_cast<int>(sizeof(redirected)),
                callerData,
                calleeData,
                socketQos,
                groupQos);
}

/** Redirects every nonempty ANSI node to the IPv4 redirect literal. */
BOOL PASCAL connect_by_name_a(SOCKET socket,
                              LPCSTR nodeName,
                              LPCSTR serviceName,
                              LPDWORD localAddressLength,
                              LPSOCKADDR localAddress,
                              LPDWORD remoteAddressLength,
                              LPSOCKADDR remoteAddress,
                              const timeval* timeout,
                              LPWSAOVERLAPPED reserved) noexcept {
    const auto call = original<decltype(&::WSAConnectByNameA)>(HookSlot::wsaConnectByNameA);
    const bool targetsRedirect = nodeName != nullptr && nodeName[0] != '\0';
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        (void)policy::deny_socket_call();
        return FALSE;
    }
    return call(socket,
                resolver::redirect_host_a(),
                serviceName,
                localAddressLength,
                localAddress,
                remoteAddressLength,
                remoteAddress,
                timeout,
                reserved);
}

/** Redirects every nonempty wide node to the IPv4 redirect literal. */
BOOL PASCAL connect_by_name_w(SOCKET socket,
                              LPWSTR nodeName,
                              LPWSTR serviceName,
                              LPDWORD localAddressLength,
                              LPSOCKADDR localAddress,
                              LPDWORD remoteAddressLength,
                              LPSOCKADDR remoteAddress,
                              const timeval* timeout,
                              LPWSAOVERLAPPED reserved) noexcept {
    const auto call = original<decltype(&::WSAConnectByNameW)>(HookSlot::wsaConnectByNameW);
    const bool targetsRedirect = nodeName != nullptr && nodeName[0] != L'\0';
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        (void)policy::deny_socket_call();
        return FALSE;
    }
    resolver::WideNode redirectedNode = resolver::redirect_node();
    return call(socket,
                redirectedNode.data(),
                serviceName,
                localAddressLength,
                localAddress,
                remoteAddressLength,
                remoteAddress,
                timeout,
                reserved);
}

/** Redirects one caller-provided IPv4 address to the exact redirect target. */
BOOL PASCAL connect_by_list(SOCKET socket,
                            PSOCKET_ADDRESS_LIST addresses,
                            LPDWORD localAddressLength,
                            LPSOCKADDR localAddress,
                            LPDWORD remoteAddressLength,
                            LPSOCKADDR remoteAddress,
                            const timeval* timeout,
                            LPWSAOVERLAPPED reserved) noexcept {
    const auto call = original<decltype(&::WSAConnectByList)>(HookSlot::wsaConnectByList);
    sockaddr_in redirectedAddress{};
    const bool hasSingleAddress =
        addresses != nullptr && addresses->iAddressCount == kAllowedAddressCount;
    const bool targetsRedirect = hasSingleAddress
                                 && policy::redirect_ipv4(addresses->Address[0].lpSockaddr,
                                                          addresses->Address[0].iSockaddrLength,
                                                          redirectedAddress);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        (void)policy::deny_socket_call();
        return FALSE;
    }

    SOCKET_ADDRESS_LIST redirectedList{};
    redirectedList.iAddressCount = kAllowedAddressCount;
    redirectedList.Address[0].lpSockaddr = reinterpret_cast<sockaddr*>(&redirectedAddress);
    redirectedList.Address[0].iSockaddrLength = static_cast<INT>(sizeof(redirectedAddress));
    return call(socket,
                &redirectedList,
                localAddressLength,
                localAddress,
                remoteAddressLength,
                remoteAddress,
                timeout,
                reserved);
}

/** Redirects one complete IPv4 multipoint destination to the exact redirect target. */
SOCKET WSAAPI join_leaf(SOCKET socket,
                        const sockaddr* name,
                        int nameLength,
                        LPWSABUF callerData,
                        LPWSABUF calleeData,
                        LPQOS socketQos,
                        LPQOS groupQos,
                        DWORD flags) noexcept {
    const auto call = original<decltype(&::WSAJoinLeaf)>(HookSlot::wsaJoinLeaf);
    sockaddr_in redirected{};
    const bool targetsRedirect = policy::redirect_ipv4(name, nameLength, redirected);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::connect, targetsRedirect, true)) {
        (void)policy::deny_socket_call();
        return INVALID_SOCKET;
    }
    return call(socket,
                reinterpret_cast<const sockaddr*>(&redirected),
                static_cast<int>(sizeof(redirected)),
                callerData,
                calleeData,
                socketQos,
                groupQos,
                flags);
}

} // namespace sunrise::client::hooks::egress::winsock::connection
