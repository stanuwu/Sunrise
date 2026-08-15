#include "aes_gcm_decrypt.h"

#include <Windows.h>

#include <array>
#include <bcrypt.h>
#include <limits>

namespace sunrise::middleware::crypto::aes_gcm {

/** Authenticates and decrypts one buffer whose tag is stored apart from its ciphertext. */
bool decrypt(std::span<const std::byte, kKeySize> key,
             std::span<const std::byte, kNonceSize> nonce,
             std::span<const std::byte> ciphertext,
             std::span<const std::byte, kTagSize> tag,
             std::span<std::byte> output) noexcept {
    if (ciphertext.size() > (std::numeric_limits<ULONG>::max)()
        || output.size() < ciphertext.size()) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    BCRYPT_KEY_HANDLE symmetricKey = nullptr;
    bool authenticated = false;
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
        authentication.pbTag = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(tag.data()));
        authentication.cbTag = static_cast<ULONG>(tag.size());
        ULONG produced = 0;
        authenticated =
            BCryptDecrypt(symmetricKey,
                          reinterpret_cast<PUCHAR>(const_cast<std::byte*>(ciphertext.data())),
                          static_cast<ULONG>(ciphertext.size()),
                          &authentication,
                          nullptr,
                          0,
                          reinterpret_cast<PUCHAR>(output.data()),
                          static_cast<ULONG>(output.size()),
                          &produced,
                          0)
                >= 0
            && produced == ciphertext.size();
        BCryptDestroyKey(symmetricKey);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return authenticated;
}

} // namespace sunrise::middleware::crypto::aes_gcm
