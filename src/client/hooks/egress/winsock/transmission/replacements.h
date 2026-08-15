#pragma once

#include "../../platform/sdk.h"

namespace sunrise::client::hooks::egress::winsock::transmission {

/** Sends contiguous bytes only to a connected exact IPv4 redirect target. */
int WSAAPI send_bytes(SOCKET socket, const char* buffer, int length, int flags) noexcept;

/** Sends buffers only to a connected exact IPv4 redirect target. */
int WSAAPI send_buffers(SOCKET socket,
                        LPWSABUF buffers,
                        DWORD bufferCount,
                        LPDWORD bytesSent,
                        DWORD flags,
                        LPWSAOVERLAPPED overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

/** Redirects an IPv4 datagram to the exact redirect target. */
int WSAAPI send_bytes_to(SOCKET socket,
                         const char* buffer,
                         int length,
                         int flags,
                         const sockaddr* destination,
                         int destinationLength) noexcept;

/** Redirects vectored IPv4 datagrams to the exact redirect target. */
int WSAAPI send_buffers_to(SOCKET socket,
                           LPWSABUF buffers,
                           DWORD bufferCount,
                           LPDWORD bytesSent,
                           DWORD flags,
                           const sockaddr* destination,
                           int destinationLength,
                           LPWSAOVERLAPPED overlapped,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

/** Redirects an explicit IPv4 message destination to the exact redirect target. */
INT PASCAL send_message(SOCKET socket,
                        LPWSAMSG message,
                        DWORD flags,
                        LPDWORD bytesSent,
                        LPWSAOVERLAPPED overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

/** Sends disconnect data only to a connected exact IPv4 redirect target. */
int WSAAPI send_disconnect(SOCKET socket, LPWSABUF outboundData) noexcept;

} // namespace sunrise::client::hooks::egress::winsock::transmission
