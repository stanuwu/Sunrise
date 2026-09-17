#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::gameplay::nat::discovery {

inline constexpr std::uint16_t kFirstPort = 3074;
inline constexpr std::uint16_t kSecondPort = 3075;
inline constexpr std::size_t kReplyCapacity = 16;

enum class Request { none, natProbe, ipDiscovery };

[[nodiscard]] Request classify(std::span<const std::byte> request) noexcept;
/**
 * Encodes the source mapping observed by the discovery socket, in host order.
 * The caller
 * must select a physically valid response source before using this codec:
 * a logical port change
 * inside a shared UDP carrier cannot establish NAT filtering or mapping.
 * @return Encoded size,
 * or zero for invalid input or insufficient output space.
 */
[[nodiscard]] std::size_t reply(std::span<const std::byte> request,
                                std::uint32_t address,
                                std::uint16_t port,
                                std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::gameplay::nat::discovery
