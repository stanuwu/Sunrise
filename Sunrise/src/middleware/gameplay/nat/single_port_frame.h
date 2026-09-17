#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::gameplay::single_port {
// Sunrise transport framing, outside the unchanged native datagram. All fields are big endian.
// Header: the four magic bytes, u8 kind, one clear byte, u16 payload length, u32 address,
// u16 port, two clear bytes.
inline constexpr std::array kMagic{std::byte{'S'}, std::byte{'R'}, std::byte{'U'}, std::byte{'1'}};
inline constexpr std::size_t kHeader = 16;
// Largest native datagram the carrier wraps, the same length the gameplay endpoint accepts on a
// bound port. encode() refuses anything longer rather than truncating it.
inline constexpr std::size_t kPayload = 1500;
inline constexpr std::size_t kCapacity = kHeader + kPayload;
enum class Kind : unsigned char { request = 1, delivery = 2 };
struct Frame {
    Kind kind{};
    std::uint32_t address{};
    std::uint16_t port{};
    std::span<const std::byte> payload{};
};
inline void put(std::span<std::byte> out, std::uint32_t value) noexcept {
    for (std::size_t i = out.size(); i; --i) {
        out[i - 1] = static_cast<std::byte>(value & 255);
        value >>= 8;
    }
}
inline std::uint32_t get(std::span<const std::byte> in) noexcept {
    std::uint32_t value{};
    for (auto b : in)
        value = (value << 8) | std::to_integer<unsigned>(b);
    return value;
}
[[nodiscard]] inline std::size_t encode(Frame frame, std::span<std::byte> out) noexcept {
    if ((frame.kind != Kind::request && frame.kind != Kind::delivery) || !frame.address
        || !frame.port || frame.payload.size() > kPayload
        || out.size() < kHeader + frame.payload.size())
        return 0;
    std::copy(kMagic.begin(), kMagic.end(), out.begin());
    out[4] = static_cast<std::byte>(frame.kind);
    out[5] = out[14] = out[15] = std::byte{};
    put(out.subspan(6, 2), static_cast<std::uint32_t>(frame.payload.size()));
    put(out.subspan(8, 4), frame.address);
    put(out.subspan(12, 2), frame.port);
    std::copy(frame.payload.begin(), frame.payload.end(), out.begin() + kHeader);
    return kHeader + frame.payload.size();
}
[[nodiscard]] inline bool decode(std::span<const std::byte> in, Frame& out) noexcept {
    if (in.size() < kHeader || in.size() > kCapacity
        || !std::equal(kMagic.begin(), kMagic.end(), in.begin()) || in[5] != std::byte{}
        || in[14] != std::byte{} || in[15] != std::byte{}
        || get(in.subspan(6, 2)) != in.size() - kHeader)
        return false;
    const auto kind = static_cast<Kind>(in[4]);
    const auto address = get(in.subspan(8, 4));
    const auto port = static_cast<std::uint16_t>(get(in.subspan(12, 2)));
    if ((kind != Kind::request && kind != Kind::delivery) || !address || !port) return false;
    out = {kind, address, port, in.subspan(kHeader)};
    return true;
}
} // namespace sunrise::middleware::gameplay::single_port
