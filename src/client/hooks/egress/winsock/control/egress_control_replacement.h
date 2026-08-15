#pragma once

#include "../../platform/sdk.h"

namespace sunrise::client::hooks::egress::winsock::control {

/** Forwards one provider control. The endpoint it configures is already loopback-only. */
int WSAAPI socket_control(SOCKET socket,
                          DWORD controlCode,
                          LPVOID input,
                          DWORD inputSize,
                          LPVOID output,
                          DWORD outputSize,
                          LPDWORD bytesReturned,
                          LPWSAOVERLAPPED overlapped,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) noexcept;

} // namespace sunrise::client::hooks::egress::winsock::control
