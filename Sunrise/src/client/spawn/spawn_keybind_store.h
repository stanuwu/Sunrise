#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::spawn {

inline constexpr std::uint32_t kNoKey = 0;

enum class Action : std::uint8_t {
    mainPlayer,
    mainCrosshair,
    projectilePlayer,
    projectileCrosshair,
    lootPlayer,
    lootCrosshair,
    count,
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::count);
inline constexpr std::uint64_t kDefaultHiddenMainTypes =
    (1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) | (1ULL << 9)
    | (1ULL << 14) | (1ULL << 22) | (1ULL << 23) | (1ULL << 24) | (1ULL << 25) | (1ULL << 28)
    | (1ULL << 63);

struct Keybinds {
    std::array<std::uint32_t, kActionCount> virtualKeys{};
    std::uint64_t hiddenMainTypes{kDefaultHiddenMainTypes};
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Keybinds get() noexcept;
bool publish(const Keybinds& keybinds) noexcept;

} // namespace sunrise::client::spawn
