#pragma once

#include "../../platform/sdk.h"

namespace sunrise::client::hooks::egress::winsock::connection {

/** Redirects an IPv4 socket connection to the exact redirect target. */
int WSAAPI connect_socket(SOCKET socket, const sockaddr* name, int nameLength) noexcept;

/** Redirects an IPv4 connection with quality-of-service data to the exact redirect target. */
int WSAAPI connect_socket_ex(SOCKET socket,
                             const sockaddr* name,
                             int nameLength,
                             LPWSABUF callerData,
                             LPWSABUF calleeData,
                             LPQOS socketQos,
                             LPQOS groupQos) noexcept;

/** Redirects every nonempty ANSI node to the IPv4 redirect literal. */
BOOL PASCAL connect_by_name_a(SOCKET socket,
                              LPCSTR nodeName,
                              LPCSTR serviceName,
                              LPDWORD localAddressLength,
                              LPSOCKADDR localAddress,
                              LPDWORD remoteAddressLength,
                              LPSOCKADDR remoteAddress,
                              const timeval* timeout,
                              LPWSAOVERLAPPED reserved) noexcept;

/** Redirects every nonempty wide node to the IPv4 redirect literal. */
BOOL PASCAL connect_by_name_w(SOCKET socket,
                              LPWSTR nodeName,
                              LPWSTR serviceName,
                              LPDWORD localAddressLength,
                              LPSOCKADDR localAddress,
                              LPDWORD remoteAddressLength,
                              LPSOCKADDR remoteAddress,
                              const timeval* timeout,
                              LPWSAOVERLAPPED reserved) noexcept;

/** Redirects one IPv4 address-list entry to the exact redirect target. */
BOOL PASCAL connect_by_list(SOCKET socket,
                            PSOCKET_ADDRESS_LIST addresses,
                            LPDWORD localAddressLength,
                            LPSOCKADDR localAddress,
                            LPDWORD remoteAddressLength,
                            LPSOCKADDR remoteAddress,
                            const timeval* timeout,
                            LPWSAOVERLAPPED reserved) noexcept;

/** Redirects an IPv4 multipoint join to the exact redirect target. */
SOCKET WSAAPI join_leaf(SOCKET socket,
                        const sockaddr* name,
                        int nameLength,
                        LPWSABUF callerData,
                        LPWSABUF calleeData,
                        LPQOS socketQos,
                        LPQOS groupQos,
                        DWORD flags) noexcept;

} // namespace sunrise::client::hooks::egress::winsock::connection
