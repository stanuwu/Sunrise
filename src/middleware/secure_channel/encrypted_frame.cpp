#include <Windows.h>

#include <array>
#include <bcrypt.h>
#include <limits>

#include "runtime.h"

namespace sunrise::middleware::secure_channel {
namespace {

/**
 * Applies authenticated AES-GCM encryption or decryption with Windows CNG.
 * @param encrypt True to seal input, false to authenticate and open it.
 * @param key Connection AES-128 key.
 * @param nonce Connection-direction frame nonce.
 * @param input Plaintext or ciphertext bytes.
 * @param output Receives transformed bytes.
 * @param tag Receives or supplies the authentication tag.
 * @return True only when the complete transform and authentication succeed.
 */
[[nodiscard]] bool crypt_gcm(bool encrypt,
                             std::span<const std::byte, state::kAesKeySize> key,
                             std::span<const std::byte, state::kBapNonceSize> nonce,
                             std::span<const std::byte> input,
                             std::span<std::byte> output,
                             std::span<std::byte, kFrameTagSize> tag) noexcept {
    if (input.size() > std::numeric_limits<ULONG>::max() || output.size() < input.size()) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE symmetricKey = nullptr;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    if (BCryptSetProperty(algorithm,
                          BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM),
                          0)
            >= 0
        && BCryptGenerateSymmetricKey(algorithm,
                                      &symmetricKey,
                                      nullptr,
                                      0,
                                      reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())),
                                      static_cast<ULONG>(key.size()),
                                      0)
               >= 0) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication;
        BCRYPT_INIT_AUTH_MODE_INFO(authentication);
        authentication.pbNonce = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(nonce.data()));
        authentication.cbNonce = static_cast<ULONG>(nonce.size());
        authentication.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        authentication.cbTag = static_cast<ULONG>(tag.size());
        ULONG transformed = 0;
        const NTSTATUS status =
            encrypt ? BCryptEncrypt(symmetricKey,
                                    reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                    static_cast<ULONG>(input.size()),
                                    &authentication,
                                    nullptr,
                                    0,
                                    reinterpret_cast<PUCHAR>(output.data()),
                                    static_cast<ULONG>(output.size()),
                                    &transformed,
                                    0)
                    : BCryptDecrypt(symmetricKey,
                                    reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                    static_cast<ULONG>(input.size()),
                                    &authentication,
                                    nullptr,
                                    0,
                                    reinterpret_cast<PUCHAR>(output.data()),
                                    static_cast<ULONG>(output.size()),
                                    &transformed,
                                    0);
        success = status >= 0 && transformed == static_cast<ULONG>(input.size());
    }
    if (symmetricKey != nullptr) {
        BCryptDestroyKey(symmetricKey);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

} // namespace

/** Seals one frame payload as tag followed by ciphertext. */
bool seal_frame(std::span<const std::byte, state::kAesKeySize> key,
                std::span<const std::byte, state::kBapNonceSize> nonce,
                std::span<const std::byte> plaintext,
                std::span<std::byte> output,
                std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kFrameTagSize + plaintext.size()) {
        return false;
    }
    auto tag = output.first<kFrameTagSize>();
    if (!crypt_gcm(true, key, nonce, plaintext, output.subspan(kFrameTagSize), tag)) {
        return false;
    }
    written = kFrameTagSize + plaintext.size();
    return true;
}

/** Authenticates and opens one tag-prefixed encrypted frame payload. */
bool open_frame(std::span<const std::byte, state::kAesKeySize> key,
                std::span<const std::byte, state::kBapNonceSize> nonce,
                std::span<const std::byte> payload,
                std::span<std::byte> output,
                std::size_t& written) noexcept {
    written = 0;
    if (payload.size() < kFrameTagSize || output.size() < payload.size() - kFrameTagSize) {
        return false;
    }
    std::array<std::byte, kFrameTagSize> tag;
    for (std::size_t index = 0; index < tag.size(); ++index) {
        tag[index] = payload[index];
    }
    if (!crypt_gcm(false, key, nonce, payload.subspan(kFrameTagSize), output, tag)) {
        return false;
    }
    written = payload.size() - kFrameTagSize;
    return true;
}

/** Increments a BAP nonce as a little-endian fixed-width counter. */
void advance_nonce(std::span<std::byte, state::kBapNonceSize> nonce) noexcept {
    for (std::byte& value : nonce) {
        value = static_cast<std::byte>(std::to_integer<std::uint8_t>(value) + 1);
        if (value != std::byte{0}) {
            return;
        }
    }
}

} // namespace sunrise::middleware::secure_channel
