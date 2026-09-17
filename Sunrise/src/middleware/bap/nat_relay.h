#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nat_punch_intro.h"

namespace sunrise::middleware::bap::nat_relay {

/** Native relay-handle marker: RVA 0xB5E580 requires signed word -2 at handle +0x12. */
inline constexpr std::uint16_t kRelayAddressFamily = 0xFFFE;

/** Native connection insertion (RVA 0x17C08F0) scans 0x3E slots before refusing admission. */
inline constexpr std::size_t kClientRelayConnectionCapacity = 62;

inline constexpr std::size_t kAddressSize = nat_punch::kAddressSize;

using SecureAddress = std::array<std::byte, kAddressSize>;

namespace initiate {
/** Body fields in order: u16 kind, one secure address, then two more big-endian u16. */
inline constexpr std::size_t kKind = 0x00;
inline constexpr std::size_t kPeerAddress = kKind + 2;
/** Two u16 the native decoder byte-swaps and Sunrise relays verbatim; their meaning is open. */
inline constexpr std::size_t kFieldA = kPeerAddress + kAddressSize;
inline constexpr std::size_t kFieldB = kFieldA + 2;
/** The native decoder refuses a body that is not exactly this long. */
inline constexpr std::size_t kBodySize = kFieldB + 2;
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
/** Length the native reader's size guard requires. Everything past the session id is padding. */
inline constexpr std::size_t kBodySize = 0xB6;
/** Wire `+0x18` u16: the declared address length. Anything but `0x56` is refused by name. */
inline constexpr std::size_t kAddressLength = 0x18;
/** Wire `+0x1A`, the REMOTE PEER's 86-byte secure address . */
inline constexpr std::size_t kRemoteAddress = kAddressLength + 2;
/** Wire `+0x9A` u16, relay UDP port. */
inline constexpr std::size_t kPort = 0x9A;
/** Wire `+0x9C` u16: `4` selects an IPv4 endpoint, `0x10` an IPv6 one. */
inline constexpr std::size_t kEndpointKind = kPort + 2;
/** The relay's IPv4, u32, directly behind the kind that selects it. */
inline constexpr std::size_t kEndpointAddress = kEndpointKind + 2;
/** The relay session id, u32, and the last field the native reader consumes. */
inline constexpr std::size_t kSessionId = kEndpointAddress + 4;
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
