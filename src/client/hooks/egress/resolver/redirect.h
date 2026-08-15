#pragma once

#include <array>
#include <string_view>

#include "../../../../core/settings/settings.h"
#include "../platform/sdk.h"
#include "../policy/policy.h"

namespace sunrise::client::hooks::egress::resolver {

/** A non-recoverable failure stops callers retrying another server. */
inline constexpr int kBlockedResolutionError = EAI_FAIL;
/** Legacy host lookup reports the documented host-not-found result. */
inline constexpr int kBlockedHostError = WSAHOST_NOT_FOUND;
/** IPv6 callers are answered on the IPv6 loopback rather than refused. */
inline constexpr char kLoopbackHost6A[] = "::1";
inline constexpr wchar_t kLoopbackHost6W[] = L"::1";

/** Sets the resolver error used by both direct and last-error contracts. */
template <typename Character>
[[nodiscard]] inline int deny_resolution(policy::NameOperation operation,
                                         const Character* name,
                                         unsigned selector) noexcept {
    policy::log_name_decision(operation,
                              false,
                              name != nullptr ? std::basic_string_view<Character>(name)
                                              : std::basic_string_view<Character>{},
                              selector);
    WSASetLastError(kBlockedResolutionError);
    return kBlockedResolutionError;
}

/** @return The external-server host, or the loopback defaults while the switch is off. */
[[nodiscard]] inline const core::settings::client::external::Settings&
redirect_settings() noexcept {
    // The default value is the loopback host, so the switch picks between two full settings, not
    // between a field and a literal.
    static const core::settings::client::external::Settings kLoopback{};
    const core::settings::client::external::Settings& configured =
        core::settings::get().client.externalServer;
    return configured.enabled ? configured : kLoopback;
}

/** @return The redirect target as a numeric host, so the original cannot emit DNS. */
[[nodiscard]] inline const char* redirect_host_a() noexcept {
    return redirect_settings().host.data();
}

/** @return The redirect target as a wide numeric host. */
[[nodiscard]] inline const wchar_t* redirect_host_w() noexcept {
    return redirect_settings().hostWide.data();
}

/** Wide node storage for the name-based connect APIs, which take a mutable node. */
using WideNode = std::array<wchar_t, core::settings::client::external::kHostCapacity>;

/** @return The redirect host copied into caller storage the original may write through. */
[[nodiscard]] inline WideNode redirect_node() noexcept {
    WideNode node{};
    const wchar_t* const host = redirect_host_w();
    for (std::size_t index = 0; index + 1 < node.size() && host[index] != L'\0'; ++index) {
        node[index] = host[index];
    }
    return node;
}

/** @return The exact IPv4 redirect target in network order. */
[[nodiscard]] inline in_addr redirect_address() noexcept {
    const std::array<unsigned char, 4> octets = policy::redirect_octets();
    in_addr value{};
    value.S_un.S_un_b = {octets[0], octets[1], octets[2], octets[3]};
    return value;
}

} // namespace sunrise::client::hooks::egress::resolver
