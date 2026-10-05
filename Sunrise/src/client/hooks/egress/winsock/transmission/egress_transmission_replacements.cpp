#include "../../internal.h"
#include "../../policy/egress_policy_logging.h"
#include "../../policy/policy.h"
#include "../single_port.h"
#include "replacements.h"

namespace sunrise::client::hooks::egress::winsock::transmission {
namespace {

/**
 * Sets a fixed byte count before a blocked overlapped send returns.
 * @param bytes Optional caller-owned byte count.
 */
void clear_bytes(LPDWORD bytes) noexcept {
    if (bytes != nullptr) {
        *bytes = 0;
    }
}

} // namespace

/** Sends contiguous bytes to a connected exact IPv4 redirect target. */
int WSAAPI send_bytes(SOCKET socket, const char* buffer, int length, int flags) noexcept {
    if (single_port::enabled(socket)) {
        return single_port::unsupported();
    }
    const auto call = original<decltype(&::send)>(HookSlot::send);
    policy::log_send_peer(socket, static_cast<std::size_t>(length >= 0 ? length : 0));
    const bool targetsRedirect = policy::has_redirect_target_peer(socket);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
        return policy::deny_socket_call();
    }
    return call(socket, buffer, length, flags);
}

/** Sends buffers to a connected exact IPv4 redirect target. */
int WSAAPI send_buffers(SOCKET socket,
                        LPWSABUF buffers,
                        DWORD bufferCount,
                        LPDWORD bytesSent,
                        DWORD flags,
                        LPWSAOVERLAPPED overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept {
    if (single_port::enabled(socket)) {
        clear_bytes(bytesSent);
        return single_port::unsupported();
    }
    const auto call = original<decltype(&::WSASend)>(HookSlot::wsaSend);
    policy::log_send_peer(socket, buffers != nullptr && bufferCount == 1 ? buffers[0].len : 0);
    const bool targetsRedirect = policy::has_redirect_target_peer(socket);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
        clear_bytes(bytesSent);
        return policy::deny_socket_call();
    }
    return call(socket, buffers, bufferCount, bytesSent, flags, overlapped, completion);
}

/** Sends disconnect data to a connected exact IPv4 redirect target. */
int WSAAPI send_disconnect(SOCKET socket, LPWSABUF outboundData) noexcept {
    const auto call = original<decltype(&::WSASendDisconnect)>(HookSlot::wsaSendDisconnect);
    const bool targetsRedirect = policy::has_redirect_target_peer(socket);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
        return policy::deny_socket_call();
    }
    return call(socket, outboundData);
}

} // namespace sunrise::client::hooks::egress::winsock::transmission
