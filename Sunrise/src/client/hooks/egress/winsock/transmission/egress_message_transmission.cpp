#include "../../internal.h"
#include "../../policy/egress_policy_logging.h"
#include "../../policy/policy.h"
#include "../single_port.h"
#include "replacements.h"

namespace sunrise::client::hooks::egress::winsock::transmission {
namespace {

/** Clears an optional send count before a policy denial. */
void clear_bytes(LPDWORD bytes) noexcept {
    if (bytes != nullptr) {
        *bytes = 0;
    }
}

} // namespace

/** Redirects one message destination to the IPv4 redirect target. */
INT PASCAL send_message(SOCKET socket,
                        LPWSAMSG message,
                        DWORD flags,
                        LPDWORD bytesSent,
                        LPWSAOVERLAPPED overlapped,
                        LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept {
    if (single_port::enabled(socket)) {
        clear_bytes(bytesSent);
        return single_port::unsupported();
    }
    const auto call = original<decltype(&::WSASendMsg)>(HookSlot::wsaSendMsg);
    if (message == nullptr) {
        clear_bytes(bytesSent);
        (void)policy::allow_socket_call(policy::SocketOperation::send, false, call != nullptr);
        return policy::deny_socket_call();
    }
    if (message->name == nullptr) {
        const bool targetsRedirect = policy::has_redirect_target_peer(socket);
        if (call == nullptr
            || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
            clear_bytes(bytesSent);
            return policy::deny_socket_call();
        }
        return call(socket, message, flags, bytesSent, overlapped, completion);
    }

    policy::log_send_target(policy::SocketOperation::send, message->name, message->namelen, 0);
    sockaddr_in redirectedAddress{};
    const bool targetsRedirect =
        policy::redirect_ipv4(message->name, message->namelen, redirectedAddress, socket);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
        clear_bytes(bytesSent);
        return policy::deny_socket_call();
    }
    WSAMSG redirectedMessage = *message;
    redirectedMessage.name = reinterpret_cast<sockaddr*>(&redirectedAddress);
    redirectedMessage.namelen = static_cast<INT>(sizeof(redirectedAddress));
    return call(socket, &redirectedMessage, flags, bytesSent, overlapped, completion);
}

} // namespace sunrise::client::hooks::egress::winsock::transmission
