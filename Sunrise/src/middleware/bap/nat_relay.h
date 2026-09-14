#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nat_punch_intro.h"

namespace sunrise::middleware::bap::nat_relay {

inline constexpr std::uint16_t kRelayAddressFamily = 0xFFFE;

inline constexpr std::size_t kClientRelayConnectionCapacity = 62;

inline constexpr std::size_t kAddressSize = nat_punch::kAddressSize;

using SecureAddress = std::array<std::byte, kAddressSize>;

namespace initiate {
inline constexpr std::size_t kBodySize = 0x5C;
inline constexpr std::size_t kKind = 0x00;
inline constexpr std::size_t kPeerAddress = 0x02;
inline constexpr std::size_t kFieldA = 0x58;
inline constexpr std::size_t kFieldB = 0x5A;
} // namespace initiate

struct InitiateRelayConnection final {
    SecureAddress peerAddress{};
    std::uint16_t kind{};
    std::uint16_t fieldA{};
    std::uint16_t fieldB{};
};

[[nodiscard]] bool encode_initiate(const InitiateRelayConnection& request,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

[[nodiscard]] bool decode_initiate(std::span<const std::byte> body,
                                   InitiateRelayConnection& request) noexcept;

namespace request_notification {
inline constexpr std::size_t kBodySize = 0xB6;
/** Wire `+0x18` u16: the declared address length. Anything but `0x56` is refused by name. */
inline constexpr std::size_t kAddressLength = 0x18;
/** Wire `+0x1A`, the REMOTE PEER's 86-byte secure address . */
inline constexpr std::size_t kRemoteAddress = 0x1A;
/** Wire `+0x9A` u16, relay UDP port. */
inline constexpr std::size_t kPort = 0x9A;
/** Wire `+0x9C` u16: `4` selects an IPv4 endpoint, `0x10` an IPv6 one. */
inline constexpr std::size_t kEndpointKind = 0x9C;
inline constexpr std::size_t kEndpointAddress = 0x9E;
inline constexpr std::size_t kSessionId = 0xA2;
/** The value wire `+0x18` must carry. */
inline constexpr std::uint16_t kAddressLengthValue = 0x56;
inline constexpr std::uint16_t kEndpointKindIpv4 = 4;
} // namespace request_notification

struct RequestRelayConnection final {
    SecureAddress remoteAddress{};
    std::uint32_t endpointAddress{};
    std::uint16_t endpointPort{};
    std::uint32_t sessionId{};
};

[[nodiscard]] bool encode_request_notification(const RequestRelayConnection& notification,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;

[[nodiscard]] bool decode_request_notification(std::span<const std::byte> body,
                                               RequestRelayConnection& notification) noexcept;

[[nodiscard]] SecureAddress
make_endpoint_address(std::uint32_t address, std::uint16_t port, std::uint16_t family) noexcept;

} // namespace sunrise::middleware::bap::nat_relay
