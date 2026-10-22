#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::gameplay::nat::discovery {

/** The two fixed native discovery ports; nothing else may bind them. */
inline constexpr std::uint16_t kFirstPort = 3074;
/** See kFirstPort. */
inline constexpr std::uint16_t kSecondPort = 3075;
/** Bound on one reply, so the longer of the two reply records fits. */
inline constexpr std::size_t kReplyCapacity = 16;

/** What a received datagram is asking for, if anything this responder answers. */
enum class Request { none, natProbe, ipDiscovery };

/** @return The request kind `request` classifies as, or `none` for anything else. */
[[nodiscard]] Request classify(std::span<const std::byte> request) noexcept;
/**
 * Composes the reply for `request`, echoing its own type/stage bytes back.
 * The caller must select a physically valid response source: a logical port change inside
 * a shared UDP carrier cannot establish NAT filtering or mapping.
 * @param address Source observed by the discovery socket, in host order.
 * @param port Source port observed by the discovery socket, in host order.
 * @return Bytes written to `output`, or zero when `request` classifies as `none`, `address` or
 * `port` is zero, or `output` is too small.
 */
[[nodiscard]] std::size_t reply(std::span<const std::byte> request,
                                std::uint32_t address,
                                std::uint16_t port,
                                std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::gameplay::nat::discovery
