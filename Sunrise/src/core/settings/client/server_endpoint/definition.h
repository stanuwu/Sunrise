#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::core::settings::client::server_endpoint {

/** Longest dotted-quad host text plus its terminator. */
inline constexpr std::size_t kHostCapacity = 16;
/** An IPv4 address is four octets. */
inline constexpr std::size_t kAddressOctets = 4;
/**
 * Default port for the TCP control link. In single-port mode this is also the one UDP
 * socket that carries discovery traffic.
 */
inline constexpr std::uint16_t kDefaultBapPort = 30974;

/** Where this client's server is, read only while `role` is `client`. */
struct Settings {
    std::array<char, kHostCapacity> host{"127.0.0.1"};
    std::array<unsigned char, kAddressOctets> address{127, 0, 0, 1};
    std::uint16_t bapPort{kDefaultBapPort};
};

} // namespace sunrise::core::settings::client::server_endpoint
