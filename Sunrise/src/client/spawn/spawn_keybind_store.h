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

struct Keybinds {
    std::array<std::uint32_t, kActionCount> virtualKeys{};
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Keybinds get() noexcept;
bool publish(const Keybinds& keybinds) noexcept;

} // namespace sunrise::client::spawn
