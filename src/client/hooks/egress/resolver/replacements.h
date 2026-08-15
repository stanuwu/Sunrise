#pragma once

#include "../platform/sdk.h"

namespace sunrise::client::hooks::egress::resolver {

/** Resolves supported ANSI forward names to the IPv4 redirect target. */
INT WSAAPI address_info_a(PCSTR node,
                          PCSTR service,
                          const ADDRINFOA* hints,
                          PADDRINFOA* result) noexcept;

/** Resolves supported wide forward names to the IPv4 redirect target. */
INT WSAAPI address_info_w(PCWSTR node,
                          PCWSTR service,
                          const ADDRINFOW* hints,
                          PADDRINFOW* result) noexcept;

/** Blocks extended ANSI address resolution. */
INT WSAAPI address_info_ex_a(PCSTR name,
                             PCSTR service,
                             DWORD nameSpace,
                             LPGUID provider,
                             const ADDRINFOEXA* hints,
                             PADDRINFOEXA* result,
                             timeval* timeout,
                             LPOVERLAPPED overlapped,
                             LPLOOKUPSERVICE_COMPLETION_ROUTINE completion,
                             LPHANDLE nameHandle) noexcept;

/** Blocks extended wide address resolution. */
INT WSAAPI address_info_ex_w(PCWSTR name,
                             PCWSTR service,
                             DWORD nameSpace,
                             LPGUID provider,
                             const ADDRINFOEXW* hints,
                             PADDRINFOEXW* result,
                             timeval* timeout,
                             LPOVERLAPPED overlapped,
                             LPLOOKUPSERVICE_COMPLETION_ROUTINE completion,
                             LPHANDLE nameHandle) noexcept;

/** Resolves supported legacy forward names to the IPv4 redirect target. */
hostent* WSAAPI host_by_name(const char* name) noexcept;

/** Blocks legacy reverse host lookup. */
hostent* WSAAPI host_by_address(const char* address, int length, int type) noexcept;

/** Blocks ANSI reverse name lookup. */
INT WSAAPI name_info_a(const SOCKADDR* address,
                       socklen_t addressLength,
                       PCHAR node,
                       DWORD nodeSize,
                       PCHAR service,
                       DWORD serviceSize,
                       INT flags) noexcept;

/** Blocks wide reverse name lookup. */
INT WSAAPI name_info_w(const SOCKADDR* address,
                       socklen_t addressLength,
                       PWCHAR node,
                       DWORD nodeSize,
                       PWCHAR service,
                       DWORD serviceSize,
                       INT flags) noexcept;

} // namespace sunrise::client::hooks::egress::resolver
