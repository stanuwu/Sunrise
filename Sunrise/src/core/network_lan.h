#pragma once

#include <array>
#include <cstdint>

namespace sunrise::core::network_lan {

// Co-op accepts only local-network peers until it has real authentication.

/** @return True for loopback, link-local and RFC 1918 private IPv4, host byte order. */
[[nodiscard]] constexpr bool lan_address(std::uint32_t address) noexcept {
    const std::uint32_t first = address >> 24U;
    const std::uint32_t second = (address >> 16U) & 0xFFU;
    return first == 10U || first == 127U || (first == 172U && (second & 0xF0U) == 16U)
           || (first == 192U && second == 168U) || (first == 169U && second == 254U);
}

/** @return The same test for a dotted-quad address in dotted order. */
[[nodiscard]] constexpr bool lan_address(const std::array<unsigned char, 4>& octets) noexcept {
    return lan_address((std::uint32_t{octets[0]} << 24U) | (std::uint32_t{octets[1]} << 16U)
                       | (std::uint32_t{octets[2]} << 8U) | std::uint32_t{octets[3]});
}

/** @return True for a LAN address or 0.0.0.0; every inbound source is still checked. */
[[nodiscard]] constexpr bool lan_bind(const std::array<unsigned char, 4>& octets) noexcept {
    return octets == std::array<unsigned char, 4>{} || lan_address(octets);
}

static_assert(lan_address(0x0A000001U) && lan_address(0xC0A80101U) && lan_address(0xAC1F0001U));
static_assert(!lan_address(0xAC200001U) && !lan_address(0x08080808U) && !lan_address(0x64400001U));

} // namespace sunrise::core::network_lan