#include "ecdh_p256.h"

#include <Windows.h>

#include <bcrypt.h>
#include <cstring>
#include <limits>

namespace sunrise::middleware::crypto::ecdh_p256 {
namespace {

/** Coordinate width of P-256. */
constexpr ULONG kCoordinateSize = 32;
/** CNG's public blob: its header, then X and Y. */
constexpr std::size_t kBlobSize = sizeof(BCRYPT_ECCKEY_BLOB) + kPublicKeySize;
/** Key size in bits that CNG names the curve by. */
constexpr ULONG kCurveBits = 256;

[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

} // namespace

KeyPair::~KeyPair() noexcept {
    reset();
}

void KeyPair::reset() noexcept {
    if (key_ != nullptr) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = nullptr;
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
        algorithm_ = nullptr;
    }
}

bool KeyPair::generate() noexcept {
    reset();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!succeeded(
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    algorithm_ = algorithm;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!succeeded(BCryptGenerateKeyPair(algorithm, &key, kCurveBits, 0))) {
        reset();
        return false;
    }
    key_ = key;
    if (!succeeded(BCryptFinalizeKeyPair(key, 0))) {
        reset();
        return false;
    }
    return true;
}

bool KeyPair::public_key(std::span<std::byte, kPublicKeySize> output) const noexcept {
    if (key_ == nullptr) {
        return false;
    }
    std::array<std::byte, kBlobSize> blob{};
    ULONG size = 0;
    if (!succeeded(BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_),
                                   nullptr,
                                   BCRYPT_ECCPUBLIC_BLOB,
                                   reinterpret_cast<PUCHAR>(blob.data()),
                                   static_cast<ULONG>(blob.size()),
                                   &size,
                                   0))
        || size != blob.size()) {
        return false;
    }
    std::memcpy(output.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kPublicKeySize);
    return true;
}

bool KeyPair::agree(std::span<const std::byte, kPublicKeySize> peer,
                    std::span<const std::byte> salt,
                    std::array<std::byte, kSecretSize>& output) const noexcept {
    if (key_ == nullptr || salt.size() > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    std::array<std::byte, kBlobSize> blob{};
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDH_PUBLIC_P256_MAGIC, kCoordinateSize};
    std::memcpy(blob.data(), &header, sizeof header);
    std::memcpy(blob.data() + sizeof header, peer.data(), kPublicKeySize);
    BCRYPT_KEY_HANDLE peerKey = nullptr;
    // Importing checks that the point lies on the curve.
    if (!succeeded(BCryptImportKeyPair(static_cast<BCRYPT_ALG_HANDLE>(algorithm_),
                                       nullptr,
                                       BCRYPT_ECCPUBLIC_BLOB,
                                       &peerKey,
                                       reinterpret_cast<PUCHAR>(blob.data()),
                                       static_cast<ULONG>(blob.size()),
                                       0))) {
        return false;
    }
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool done = false;
    if (succeeded(
            BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peerKey, &secret, 0))) {
        std::array<BCryptBuffer, 2> parameters{{
            {static_cast<ULONG>(sizeof(BCRYPT_SHA256_ALGORITHM)),
             KDF_HASH_ALGORITHM,
             const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)},
            {static_cast<ULONG>(salt.size()),
             KDF_SECRET_APPEND,
             const_cast<std::byte*>(salt.data())},
        }};
        BCryptBufferDesc description{
            BCRYPTBUFFER_VERSION, static_cast<ULONG>(parameters.size()), parameters.data()};
        ULONG size = 0;
        done = succeeded(BCryptDeriveKey(secret,
                                         BCRYPT_KDF_HASH,
                                         &description,
                                         reinterpret_cast<PUCHAR>(output.data()),
                                         static_cast<ULONG>(output.size()),
                                         &size,
                                         0))
               && size == output.size();
        BCryptDestroySecret(secret);
    }
    BCryptDestroyKey(peerKey);
    return done;
}

} // namespace sunrise::middleware::crypto::ecdh_p256