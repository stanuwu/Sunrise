#include "egress_dns_replacements.h"

#include <intrin.h>
#include <string_view>

#include "../internal.h"
#include "../policy/policy.h"

namespace sunrise::client::hooks::egress::dns {
namespace {

/** DNS refusal is the SDK status for a resolver that rejects the request. */
constexpr DNS_STATUS kBlockedDnsStatus = DNS_ERROR_RCODE_REFUSED;
/** Wide substitute name; the resolver answers it from the local table. */
constexpr wchar_t kLoopbackNameW[] = L"localhost";
/** Narrow substitute name used by the ANSI and UTF-8 entry points. */
constexpr char kLoopbackNameA[] = "localhost";
/** Cache and local tables only, so a substituted query can never reach the wire. */
constexpr DWORD kLocalOnlyOptions = DNS_QUERY_NO_WIRE_QUERY | DNS_QUERY_NO_NETBT;
/** Caller flags that would defeat the local answer. Together they give no records at all. */
constexpr DWORD kConflictingOptions = DNS_QUERY_BYPASS_CACHE | DNS_QUERY_NO_LOCAL_NAME
                                      | DNS_QUERY_NO_HOSTS_FILE | DNS_QUERY_WIRE_ONLY;

/**
 * A caller-supplied server list clashes with a cache-only query. The resolver then returns
 * ERROR_INVALID_PARAMETER instead of answering locally.
 * @param extra Caller resolver state; a server list when present.
 * @return Options that can be answered locally.
 */
[[nodiscard]] DWORD local_options(DWORD options, PVOID extra) noexcept {
    const DWORD cleared = options & ~kConflictingOptions;
    return extra != nullptr ? cleared : cleared | kLocalOnlyOptions;
}

/**
 * An address literal needs no lookup and cannot name a remote host, so it is passed on
 * untouched. Replacing it is what produced the rejected query.
 * @return True when the caller already asked for a numeric address.
 */
template <typename Character> [[nodiscard]] bool is_numeric(const Character* name) noexcept {
    return name != nullptr && ((name[0] >= '0' && name[0] <= '9') || name[0] == ':');
}

/** @return A view of the name, or an empty view when it is null. */
template <typename Character>
[[nodiscard]] std::basic_string_view<Character> view(const Character* name) noexcept {
    return name != nullptr ? std::basic_string_view<Character>(name)
                           : std::basic_string_view<Character>{};
}

/** @return The refusal status after recording one denied resolver query. */
template <typename Character>
[[nodiscard]] DNS_STATUS deny_query(const Character* name, WORD type, const void* caller) noexcept {
    policy::log_name_decision(policy::NameOperation::dns, false, view(name), type, 0, caller);
    return kBlockedDnsStatus;
}

/** @return The resolver's own status, after recording one substituted query. */
template <typename Character>
[[nodiscard]] DNS_STATUS
allow_query(DNS_STATUS status, const Character* name, WORD type, const void* caller) noexcept {
    policy::log_name_decision(
        policy::NameOperation::dns, true, view(name), type, static_cast<unsigned>(status), caller);
    return status;
}

/** Clears pointer outputs before returning the local DNS refusal. */
void clear_query_outputs(PDNS_RECORD* results, PVOID* reserved) noexcept {
    if (results != nullptr) {
        *results = nullptr;
    }
    if (reserved != nullptr) {
        *reserved = nullptr;
    }
}

} // namespace

/** Answers one narrow-character query from the local loopback name. */
DNS_STATUS WINAPI query_a(PCSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept {
    const void* const caller = _ReturnAddress();
    clear_query_outputs(results, reserved);
    const auto call = original<decltype(&::DnsQuery_A)>(HookSlot::dnsQueryA);
    if (name == nullptr || name[0] == '\0' || results == nullptr || call == nullptr) {
        return deny_query(name, type, caller);
    }
    if (is_numeric(name)) {
        return allow_query(call(name, type, options, extra, results, reserved), name, type, caller);
    }
    return allow_query(
        call(kLoopbackNameA, type, local_options(options, extra), extra, results, reserved),
        name,
        type,
        caller);
}

/** Answers one wide-character query from the local loopback name. */
DNS_STATUS WINAPI query_w(PCWSTR name,
                          WORD type,
                          DWORD options,
                          PVOID extra,
                          PDNS_RECORD* results,
                          PVOID* reserved) noexcept {
    const void* const caller = _ReturnAddress();
    clear_query_outputs(results, reserved);
    const auto call = original<decltype(&::DnsQuery_W)>(HookSlot::dnsQueryW);
    if (name == nullptr || name[0] == L'\0' || results == nullptr || call == nullptr) {
        return deny_query(name, type, caller);
    }
    if (is_numeric(name)) {
        return allow_query(call(name, type, options, extra, results, reserved), name, type, caller);
    }
    return allow_query(
        call(kLoopbackNameW, type, local_options(options, extra), extra, results, reserved),
        name,
        type,
        caller);
}

/** Answers one UTF-8 query from the local loopback name. */
DNS_STATUS WINAPI query_utf8(PCSTR name,
                             WORD type,
                             DWORD options,
                             PVOID extra,
                             PDNS_RECORD* results,
                             PVOID* reserved) noexcept {
    const void* const caller = _ReturnAddress();
    clear_query_outputs(results, reserved);
    const auto call = original<decltype(&::DnsQuery_UTF8)>(HookSlot::dnsQueryUtf8);
    if (name == nullptr || name[0] == '\0' || results == nullptr || call == nullptr) {
        return deny_query(name, type, caller);
    }
    if (is_numeric(name)) {
        return allow_query(call(name, type, options, extra, results, reserved), name, type, caller);
    }
    return allow_query(
        call(kLoopbackNameA, type, local_options(options, extra), extra, results, reserved),
        name,
        type,
        caller);
}

/** Answers one structured query from the local loopback name. */
DNS_STATUS WINAPI query_ex(PDNS_QUERY_REQUEST request,
                           PDNS_QUERY_RESULT results,
                           PDNS_QUERY_CANCEL cancel) noexcept {
    const void* const caller = _ReturnAddress();
    const auto call = original<decltype(&::DnsQueryEx)>(HookSlot::dnsQueryEx);
    if (request == nullptr || request->QueryName == nullptr || call == nullptr) {
        if (results != nullptr) {
            results->QueryStatus = kBlockedDnsStatus;
            results->QueryOptions = 0;
            results->pQueryRecords = nullptr;
            results->Reserved = nullptr;
        }
        if (cancel != nullptr) {
            *cancel = {};
        }
        return deny_query(request != nullptr ? request->QueryName : nullptr,
                          request != nullptr ? request->QueryType : 0,
                          caller);
    }
    DNS_QUERY_REQUEST forwarded = *request;
    if (!is_numeric(request->QueryName)) {
        forwarded.QueryName = kLoopbackNameW;
        forwarded.QueryOptions =
            local_options(static_cast<DWORD>(request->QueryOptions), request->pDnsServerList);
    }
    return allow_query(
        call(&forwarded, results, cancel), request->QueryName, request->QueryType, caller);
}

/** Rejects one raw DNS query without scheduling its completion callback. */
DNS_STATUS WINAPI query_raw(DNS_QUERY_RAW_REQUEST* request, DNS_QUERY_RAW_CANCEL* cancel) noexcept {
    (void)request;
    if (cancel != nullptr) {
        *cancel = {};
    }
    return deny_query<char>(nullptr, 0, _ReturnAddress());
}

} // namespace sunrise::client::hooks::egress::dns
