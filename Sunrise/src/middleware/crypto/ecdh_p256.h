#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::ecdh_p256 {

/** A public key as sent: X then Y, 32 big-endian bytes each. */
inline constexpr std::size_t kPublicKeySize = 64;
/** Bytes of one derived secret: SHA-256 of the shared point and the caller's salt. */
inline constexpr std::size_t kSecretSize = 32;

/** One ephemeral P-256 key pair. It never leaves this object. */
class KeyPair final {
public:
    KeyPair() noexcept = default;
    ~KeyPair() noexcept;
    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;

    /** Makes a fresh pair, replacing any previous one. @return False when Windows refused. */
    [[nodiscard]] bool generate() noexcept;
    /** Destroys the pair. */
    void reset() noexcept;
    /** Writes the public key. @return False when no pair exists. */
    [[nodiscard]] bool public_key(std::span<std::byte, kPublicKeySize> output) const noexcept;
    /**
     * Agrees on a secret with a peer's public key, then hashes it with `salt` appended.
     * @return False for an invalid peer key or when no pair exists.
     */
    [[nodiscard]] bool agree(std::span<const std::byte, kPublicKeySize> peer,
                             std::span<const std::byte> salt,
                             std::array<std::byte, kSecretSize>& output) const noexcept;

private:
    void* algorithm_{};
    void* key_{};
};

} // namespace sunrise::middleware::crypto::ecdh_p256