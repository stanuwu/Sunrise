#pragma once

#include "../runtime/state.h"

namespace sunrise::state::account {

/**
 * Public interoperability wrapping material shared by native primary and activity channels.
 * This is not credential authentication. Every connection's session key, nonce and envelope
 * IV remain independently generated.
 */
[[nodiscard]] inline SignOnState shared_channel_material() noexcept {
    SignOnState output{};
    // Fixed material, ASCII "SUNRISE6", stepped with the 13/7/17 xorshift64 triple. Bits 24-31
    // supply each byte, preserving the existing deterministic wire material.
    const auto fill = [](auto& bytes, std::uint32_t sequence) {
        std::uint64_t value = 0x53554E5249534536ULL ^ (static_cast<std::uint64_t>(sequence) << 32);
        for (auto& byte : bytes) {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
            byte = static_cast<std::byte>(value >> 24);
        }
    };
    fill(output.encryptionKey, 0);
    fill(output.authenticationKey, 1);
    return output;
}
} // namespace sunrise::state::account
