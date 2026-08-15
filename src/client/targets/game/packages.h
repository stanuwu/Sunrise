#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::targets::game::packages {

/** Package block keys are AES-128. */
inline constexpr std::size_t kBlockKeySize = 16;
/** Package block encryption uses a 12-byte AES-GCM nonce. */
inline constexpr std::size_t kNonceSize = 12;

/**
 * Installed key table read by the package block decoder.
 * Its bytes are proprietary. They are read at use time and never copied into State, cache, capture
 * or logs.
 */
struct KeyTable {
    std::array<std::byte, kBlockKeySize> alternateKey{};
    std::array<std::byte, kBlockKeySize> identityConstant{};
    std::array<std::byte, kNonceSize> nonceBase{};
};

static_assert(sizeof(KeyTable) == 2 * kBlockKeySize + kNonceSize);

/** Resolved package key-table location. */
struct Targets {
    /** First byte of the installed key table. */
    std::byte* keyTable{};
};

/** Clears the published package target group. */
void clear() noexcept;

/** @return Process-local package target group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after the key table is published. */
[[nodiscard]] bool is_resolved() noexcept;

/**
 * Copies the installed key table for one bounded use.
 * @param table Receives the borrowed key material.
 * @return True when the resolved table is completely readable.
 */
[[nodiscard]] bool read(KeyTable& table) noexcept;

} // namespace sunrise::client::targets::game::packages
