#pragma once

#include <cstddef>

namespace sunrise::client::hooks::network::investment::low_arena {

/** One array payload carved from the low arena, which owns it for the process lifetime. */
struct Allocation {
    std::byte* base{};
    std::size_t size{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return base != nullptr && size != 0;
    }
};

/** Reserves the arena once, at process attach, while low addresses are still free. */
void reserve() noexcept;

/**
 * Carves one array payload from the arena.
 * The caller holds the socket menu routing lock; the arena has no lock of its own.
 * @param size Payload bytes.
 * @return The payload, or an empty allocation when the arena is missing or full.
 */
[[nodiscard]] Allocation allocate(std::size_t size) noexcept;

} // namespace sunrise::client::hooks::network::investment::low_arena
