#include "net_addr.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace sunrise::middleware::gameplay::descriptor {
namespace {
using Address = std::array<std::byte, kNetAddrSize>;
std::uint32_t network(std::span<const std::byte> bytes) noexcept {
    std::uint32_t value{};
    for (const auto byte : bytes) {
        value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
    }
    return value;
}
bool prefix(const Address& address, std::string_view text) noexcept {
    return std::equal(text.begin(), text.end(), address.begin(), [](char letter, std::byte byte) {
        return static_cast<std::byte>(letter) == byte;
    });
}
bool zero_tail(const Address& address, std::size_t start) noexcept {
    return std::all_of(address.begin() + static_cast<std::ptrdiff_t>(start),
                       address.end() - 1,
                       [](std::byte byte) { return byte == std::byte{}; });
}
} // namespace

std::uint8_t net_addr_method(const Address& address) noexcept {
    return std::to_integer<std::uint8_t>(address.back());
}
bool net_addr_is_steam_text(const Address& address) noexcept {
    return prefix(address, "steamid:");
}

bool net_addr_endpoint(const Address& address, std::uint32_t& ipv4, std::uint16_t& port) noexcept {
    if (net_addr_method(address) > 7 || net_addr_is_steam_text(address)) {
        return false;
    }
    const auto bytes = std::span(address);
    if (prefix(address, "DRCT")) {
        if (net_addr_method(address) != 0 || !zero_tail(address, 12)) {
            return false;
        }
        const auto host = network(bytes.subspan(4, 4));
        const auto service = static_cast<std::uint16_t>(network(bytes.subspan(10, 2)));
        if (host == 0 || service == 0) {
            return false;
        }
        ipv4 = host;
        port = service;
        return true;
    }
    // Native discovery publishes open, moderate or strict NAT; none changes the carrier layout.
    if (address[40] < std::byte{1} || address[40] > std::byte{3} || !zero_tail(address, 41)) {
        return false;
    }
    for (const std::size_t offset : {std::size_t{0}, std::size_t{30}}) {
        const auto host = network(bytes.subspan(offset, 4));
        const auto service =
            static_cast<std::uint16_t>(std::to_integer<unsigned>(address[offset + 4])
                                       | (std::to_integer<unsigned>(address[offset + 5]) << 8U));
        if (host == 0 || service == 0) {
            continue;
        }
        ipv4 = host;
        port = service;
        return true;
    }
    return false;
}

std::size_t net_addr_endpoints(const Address& address, std::span<PeerEndpoint> output) noexcept {
    std::uint32_t ipv4{};
    std::uint16_t port{};
    if (!net_addr_endpoint(address, ipv4, port)) {
        return 0;
    }
    std::size_t count{};
    auto append = [&](PeerEndpoint endpoint) {
        if (unicast_endpoint(endpoint) && count < output.size()
            && std::find(
                   output.begin(), output.begin() + static_cast<std::ptrdiff_t>(count), endpoint)
                   == output.begin() + static_cast<std::ptrdiff_t>(count)) {
            output[count++] = endpoint;
        }
    };
    if (prefix(address, "DRCT")) {
        append({ipv4, port});
    } else {
        for (std::size_t offset = 0; offset <= 30; offset += 6) {
            append({network(std::span(address).subspan(offset, 4)),
                    static_cast<std::uint16_t>(
                        std::to_integer<unsigned>(address[offset + 4])
                        | (std::to_integer<unsigned>(address[offset + 5]) << 8U))});
        }
    }
    return count;
}

NetAddrNormalisation normalize_net_addr_ipv4(const Address& primary,
                                             const Address& alternate,
                                             Address& output) noexcept {
    std::uint32_t ipv4{};
    std::uint16_t port{};
    if (net_addr_method(primary) < 6 && net_addr_endpoint(primary, ipv4, port)) {
        output = primary;
        return NetAddrNormalisation::kept;
    }
    if (net_addr_method(alternate) < 6 && net_addr_endpoint(alternate, ipv4, port)) {
        output = alternate;
        return NetAddrNormalisation::alternate;
    }
    const Address* packed = net_addr_endpoint(primary, ipv4, port)     ? &primary
                            : net_addr_endpoint(alternate, ipv4, port) ? &alternate
                                                                       : nullptr;
    if (packed) {
        // Select the IPv4 transport without replacing the client's public mapping, candidate
        // list or NAT type with its first local address. Those bytes distinguish remote peers.
        output = *packed;
        output.back() = std::byte{};
        return NetAddrNormalisation::rebuilt;
    }
    return NetAddrNormalisation::unavailable;
}

const char* net_addr_normalisation_name(NetAddrNormalisation rule) noexcept {
    switch (rule) {
    case NetAddrNormalisation::kept:
        return "kept";
    case NetAddrNormalisation::alternate:
        return "alternate";
    case NetAddrNormalisation::rebuilt:
        return "rebuilt";
    case NetAddrNormalisation::unavailable:
        break;
    }
    return "unavailable";
}

} // namespace sunrise::middleware::gameplay::descriptor
