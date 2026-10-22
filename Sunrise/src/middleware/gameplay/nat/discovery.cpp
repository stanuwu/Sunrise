#include "discovery.h"

#include <algorithm>

namespace sunrise::middleware::gameplay::nat::discovery {
namespace {
// Native NAT probes have four stages; IP-discovery messages use the 30-39 family.
// A probe request is two big-endian u16, the type and the stage index.
constexpr std::size_t kNatRequestSize = 4;
// Its reply echoes both, then carries the masked address and masked port. The remaining bytes of
// the fixed record stay clear.
constexpr std::size_t kNatReplySize = 16;
constexpr std::uint16_t kNatProbeType = 1;
constexpr std::uint16_t kFirstProbeStage = 1;
constexpr std::uint16_t kLastProbeStage = 4;
// The obfuscation the native receiver undoes: it XORs the reported address with the full word and
// the reported port with its low half.
constexpr std::uint32_t kNatAddressMask = 0x76C3F6BCU;
constexpr std::uint16_t kNatPortMask = 0xF6BCU;
// An IP-discovery request is its type byte and two bytes this responder does not read.
constexpr std::size_t kIpRequestSize = 3;
// Its reply is the echoed type byte, a little-endian family, a big-endian address and a
// little-endian port.
constexpr std::size_t kIpReplySize = 9;
constexpr unsigned kFirstIpDiscoveryType = 30;
constexpr unsigned kLastIpDiscoveryType = 39;
// AF_INET, the only family this responder reports.
constexpr std::uint16_t kIpv4AddressFamily = 2;
// reply() writes into a caller buffer sized by the header's bound, so it must cover both modes.
static_assert(kReplyCapacity >= kNatReplySize && kReplyCapacity >= kIpReplySize);

std::uint16_t big_u16(std::span<const std::byte> input, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset]) << 8U)
           | static_cast<std::uint16_t>(std::to_integer<unsigned>(input[offset + 1]));
}
void big_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 8U);
    output[offset + 1] = static_cast<std::byte>(value);
}
void little_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value);
    output[offset + 1] = static_cast<std::byte>(value >> 8U);
}
void big_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) {
        output[offset + i] = static_cast<std::byte>(value >> ((3U - i) * 8U));
    }
}
} // namespace

Request classify(std::span<const std::byte> request) noexcept {
    if (request.size() == kNatRequestSize && big_u16(request, 0) == kNatProbeType) {
        const auto index = big_u16(request, 2);
        if (index >= kFirstProbeStage && index <= kLastProbeStage) {
            return Request::natProbe;
        }
    }
    if (request.size() == kIpRequestSize) {
        const auto type = std::to_integer<unsigned>(request.front());
        if (type >= kFirstIpDiscoveryType && type <= kLastIpDiscoveryType) {
            return Request::ipDiscovery;
        }
    }
    return Request::none;
}

std::size_t reply(std::span<const std::byte> request,
                  std::uint32_t address,
                  std::uint16_t port,
                  std::span<std::byte> output) noexcept {
    const auto kind = classify(request);
    if (kind == Request::none || !address || !port) {
        return 0;
    }
    const std::size_t size = kind == Request::natProbe ? kNatReplySize : kIpReplySize;
    if (output.size() < size) {
        return 0;
    }
    // Request/output may alias; preserve the only four request bytes the reply echoes.
    const auto type = request.front();
    const auto index = kind == Request::natProbe ? big_u16(request, 2) : std::uint16_t{};
    std::fill_n(output.begin(), size, std::byte{});
    if (kind == Request::natProbe) {
        big_u16(output, 0, kNatProbeType);
        big_u16(output, 2, index);
        big_u32(output, 4, address ^ kNatAddressMask);
        big_u16(output, 8, port ^ kNatPortMask);
    } else {
        output[0] = type;
        little_u16(output, 1, kIpv4AddressFamily);
        big_u32(output, 3, address);
        little_u16(output, 7, port);
    }
    return size;
}

} // namespace sunrise::middleware::gameplay::nat::discovery
