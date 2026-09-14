#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::core::settings::client::server_endpoint {

inline constexpr std::size_t kHostCapacity = 16;
inline constexpr std::size_t kAddressOctets = 4;
inline constexpr std::uint16_t kDefaultBapPort = 30974;

/** Where this client's server is, read ONLY while `role` is `client`. */
struct Settings {
    std::array<char, kHostCapacity> host{"127.0.0.1"};
    std::array<unsigned char, kAddressOctets> address{127, 0, 0, 1};
    std::uint16_t bapPort{kDefaultBapPort};
};

} // namespace sunrise::core::settings::client::server_endpoint
