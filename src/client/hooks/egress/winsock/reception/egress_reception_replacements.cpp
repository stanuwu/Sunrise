#include "egress_reception_replacements.h"

#include "../../../../../core/logging/log.h"
#include "../../internal.h"
#include "../../policy/policy.h"

namespace sunrise::client::hooks::egress::winsock::reception {
namespace {

/** A missing original keeps receive calls fail-closed. */
constexpr std::string_view kReceiveDeny = "ev=egress stage=receive target=unverified result=deny";

/** Reports a receive that cannot reach the original. Allowed receives run per packet. */
[[nodiscard]] bool allow_receive(bool originalAvailable) noexcept {
    if (!originalAvailable) {
        core::log::write(core::log::Channel::client, core::log::Level::warn, kReceiveDeny);
    }
    return originalAvailable;
}

} // namespace

/** @return Original recv result, or SOCKET_ERROR with WSAEACCES before publication. */
int WSAAPI receive_bytes(SOCKET socket, char* buffer, int length, int flags) noexcept {
    const auto call = original<decltype(&::recv)>(HookSlot::recv);
    return allow_receive(call != nullptr) ? call(socket, buffer, length, flags)
                                          : policy::deny_socket_call();
}

/** @return Original WSARecv result, or SOCKET_ERROR with WSAEACCES before publication. */
int WSAAPI receive_buffers(SOCKET socket,
                           LPWSABUF buffers,
                           DWORD bufferCount,
                           LPDWORD bytesReceived,
                           LPDWORD flags,
                           LPWSAOVERLAPPED overlapped,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept {
    const auto call = original<decltype(&::WSARecv)>(HookSlot::wsaRecv);
    return allow_receive(call != nullptr)
               ? call(socket, buffers, bufferCount, bytesReceived, flags, overlapped, completion)
               : policy::deny_socket_call();
}

/** @return Original recvfrom result, or SOCKET_ERROR with WSAEACCES before publication. */
int WSAAPI receive_bytes_from(SOCKET socket,
                              char* buffer,
                              int length,
                              int flags,
                              sockaddr* source,
                              int* sourceLength) noexcept {
    const auto call = original<decltype(&::recvfrom)>(HookSlot::recvFrom);
    return allow_receive(call != nullptr)
               ? call(socket, buffer, length, flags, source, sourceLength)
               : policy::deny_socket_call();
}

/** @return Original WSARecvFrom result, or SOCKET_ERROR with WSAEACCES before publication. */
int WSAAPI receive_buffers_from(SOCKET socket,
                                LPWSABUF buffers,
                                DWORD bufferCount,
                                LPDWORD bytesReceived,
                                LPDWORD flags,
                                sockaddr* source,
                                LPINT sourceLength,
                                LPWSAOVERLAPPED overlapped,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept {
    const auto call = original<decltype(&::WSARecvFrom)>(HookSlot::wsaRecvFrom);
    return allow_receive(call != nullptr) ? call(socket,
                                                 buffers,
                                                 bufferCount,
                                                 bytesReceived,
                                                 flags,
                                                 source,
                                                 sourceLength,
                                                 overlapped,
                                                 completion)
                                          : policy::deny_socket_call();
}

} // namespace sunrise::client::hooks::egress::winsock::reception
