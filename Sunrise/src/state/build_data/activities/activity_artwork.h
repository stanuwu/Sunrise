#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::state::build_data::activities {
/** Numeric slots into the extracted artwork table; zero has no image. */
enum class Icon : std::uint8_t {};
inline constexpr std::size_t kIconCount = 41;
struct Artwork {
    std::uint32_t tag{};
    std::uint16_t width{}, height{};
    /** Native RGBA8 pixels. No game-owned texture/resource pointers cross this boundary. */
    std::vector<std::byte> pixels{};
};
/** Publishes validated pixel buffers once from the investment worker, transferring ownership. */
[[nodiscard]] bool publish_artwork(std::array<Artwork, kIconCount>&& rows) noexcept;
/** @return Immutable process-lifetime buffers, or an empty span before publication. */
[[nodiscard]] std::span<const Artwork> artwork() noexcept;
} // namespace sunrise::state::build_data::activities
