#include "egress_extension_replacements.h"

#include "../internal.h"
#include "../policy/policy.h"

namespace sunrise::client::hooks::egress::extensions {
/** Transmits file data to a connected exact IPv4 redirect-target peer. */
BOOL PASCAL transmit_file(SOCKET socket,
                          HANDLE file,
                          DWORD bytesToWrite,
                          DWORD bytesPerSend,
                          LPOVERLAPPED overlapped,
                          LPTRANSMIT_FILE_BUFFERS buffers,
                          DWORD reserved) noexcept {
    const auto call = original<decltype(&::TransmitFile)>(HookSlot::transmitFile);
    const bool targetsRedirect = policy::has_redirect_target_peer(socket);
    if (call == nullptr
        || !policy::allow_socket_call(policy::SocketOperation::send, targetsRedirect, true)) {
        (void)policy::deny_socket_call();
        return FALSE;
    }
    return call(socket, file, bytesToWrite, bytesPerSend, overlapped, buffers, reserved);
}

} // namespace sunrise::client::hooks::egress::extensions
