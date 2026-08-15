#pragma once

#include "../platform/sdk.h"

namespace sunrise::client::hooks::egress::dns {

/** Blocks an ANSI DNS query. */
DNS_STATUS WINAPI query_a(PCSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept;

/** Blocks a wide DNS query. */
DNS_STATUS WINAPI query_w(PCWSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept;

/** Blocks a UTF-8 DNS query. */
DNS_STATUS WINAPI query_utf8(PCSTR name,
                             WORD type,
                             DWORD options,
                             PVOID extra,
                             PDNS_RECORD* results,
                             PVOID* reserved) noexcept;

/** Blocks an extended DNS query. */
DNS_STATUS WINAPI query_ex(PDNS_QUERY_REQUEST request,
                           PDNS_QUERY_RESULT results,
                           PDNS_QUERY_CANCEL cancel) noexcept;

/** Blocks a raw DNS query without scheduling its callback. */
DNS_STATUS WINAPI query_raw(DNS_QUERY_RAW_REQUEST* request, DNS_QUERY_RAW_CANCEL* cancel) noexcept;

} // namespace sunrise::client::hooks::egress::dns
