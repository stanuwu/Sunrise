#include "egress_control_replacement.h"

#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../internal.h"
#include "../../policy/policy.h"

namespace sunrise::client::hooks::egress::winsock::control {
namespace {

/**
 * Extension pointers are the one control with no loopback equivalent: the provider returns
 * ConnectEx, WSASendMsg and WSARecvMsg entry points that no detour covers. It is forwarded like
 * any other control and reported, because the fix is to replace those entry points, not refuse it.
 */
constexpr DWORD kExtensionPointerControl = SIO_GET_EXTENSION_FUNCTION_POINTER;

} // namespace

/** Forwards one provider control; the endpoint it configures is already loopback-only. */
int WSAAPI socket_control(SOCKET socket,
                          DWORD controlCode,
                          LPVOID input,
                          DWORD inputSize,
                          LPVOID output,
                          DWORD outputSize,
                          LPDWORD bytesReturned,
                          LPWSAOVERLAPPED overlapped,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept {
    const auto call = original<decltype(&::WSAIoctl)>(HookSlot::wsaIoctl);
    if (call == nullptr) {
        if (bytesReturned != nullptr) {
            *bytesReturned = 0;
        }
        policy::log_name_decision(
            policy::NameOperation::control, false, std::string_view{}, controlCode);
        return policy::deny_socket_call();
    }
    if (controlCode == kExtensionPointerControl) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=egress stage=control target=unguarded action=allow result=ok");
    }
    policy::log_name_decision(
        policy::NameOperation::control, true, std::string_view{}, controlCode);
    return call(socket,
                controlCode,
                input,
                inputSize,
                output,
                outputSize,
                bytesReturned,
                overlapped,
                completion);
}

} // namespace sunrise::client::hooks::egress::winsock::control
