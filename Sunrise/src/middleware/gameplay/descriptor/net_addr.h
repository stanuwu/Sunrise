#pragma once

#include <span>

#include "join_descriptor.h"

namespace sunrise::middleware::gameplay::descriptor {

enum class NetAddrNormalisation { unavailable, kept, alternate, rebuilt };

struct PeerEndpoint {
    std::uint32_t address{};
    std::uint16_t port{};
    bool operator==(const PeerEndpoint&) const = default;
};
/** Five local candidates and one public mapping, retaining their actual ports. */
inline constexpr std::size_t kPeerEndpointCount = 6;
/** One candidate is a four-byte IPv4 followed by a little-endian port. */
inline constexpr std::size_t kPeerEndpointStride = 6;
/** The last of those candidates is the peer's public mapping. */
inline constexpr std::size_t kPublicEndpointOffset = kPeerEndpointStride * (kPeerEndpointCount - 1);
/** First octet 224 and above is multicast or reserved, so no peer is reachable there. */
inline constexpr std::uint32_t kFirstUnroutableOctet = 224;
[[nodiscard]] std::size_t net_addr_endpoints(const std::array<std::byte, kNetAddrSize>& address,
                                             std::span<PeerEndpoint> output) noexcept;
[[nodiscard]] inline constexpr bool unicast_endpoint(PeerEndpoint endpoint) noexcept {
    const auto first = endpoint.address >> 24U;
    return endpoint.port != 0 && first != 0 && first < kFirstUnroutableOctet;
}

[[nodiscard]] bool
net_addr_is_steam_text(const std::array<std::byte, kNetAddrSize>& address) noexcept;
[[nodiscard]] std::uint8_t
net_addr_method(const std::array<std::byte, kNetAddrSize>& address) noexcept;
/** Reads the supported packed or DRCT form, leaving outputs unchanged on refusal. */
[[nodiscard]] bool net_addr_endpoint(const std::array<std::byte, kNetAddrSize>& address,
                                     std::uint32_t& ipv4,
                                     std::uint16_t& port) noexcept;
/** Both candidates must be actual publications from the same account. */
[[nodiscard]] NetAddrNormalisation
normalize_net_addr_ipv4(const std::array<std::byte, kNetAddrSize>& primary,
                        const std::array<std::byte, kNetAddrSize>& alternate,
                        std::array<std::byte, kNetAddrSize>& output) noexcept;
/** Names the rule one normalisation applied, for the identity report. */
[[nodiscard]] const char* net_addr_normalisation_name(NetAddrNormalisation rule) noexcept;

} // namespace sunrise::middleware::gameplay::descriptor
