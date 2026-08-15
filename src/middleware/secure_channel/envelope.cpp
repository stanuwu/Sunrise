#include <Windows.h>

#include <array>
#include <bcrypt.h>

#include "../encoding/byte_order.h"
#include "runtime.h"

namespace sunrise::middleware::secure_channel {
namespace {

/** Service-26 plaintext packs a 12-byte nonce and 16-byte key plus padding. */
constexpr std::size_t kPlaintextSize = 32;
/** AES-CBC block-sized IV bytes. */
constexpr std::size_t kIvSize = 16;
/** Padded service-26 AES-CBC ciphertext bytes. */
constexpr std::size_t kCiphertextSize = 32;
/** SHA-256 HMAC output bytes appended to service 26. */
constexpr std::size_t kMacSize = 32;
/** Fixed service-26 envelope field offsets. */
constexpr std::size_t kEnvelopeLengthOffset = 0;
constexpr std::size_t kEnvelopeIvOffset = kEnvelopeLengthOffset + encoding::kU32Size;
constexpr std::size_t kEnvelopeCiphertextOffset = kEnvelopeIvOffset + kIvSize;
constexpr std::size_t kAuthenticatedSize = kEnvelopeCiphertextOffset + kCiphertextSize;
constexpr std::uint32_t kEnvelopePayloadSize =
    static_cast<std::uint32_t>(kServerHelloEnvelopeSize - encoding::kU32Size);
static_assert(kServerHelloEnvelopeSize == kAuthenticatedSize + kMacSize);
/** 4 PKCS#7 bytes pad the 28-byte nonce and key payload to 2 AES blocks. */
constexpr std::byte kPaddingByte{0x04};

/**
 * Encrypts the fixed service-26 plaintext with Windows AES-CBC.
 * @param key AES-128 session-envelope key.
 * @param plaintext 2 padded AES blocks.
 * @param iv Fixed-size IV.
 * @param ciphertext Receives 2 encrypted blocks.
 * @return True when Windows CNG transforms the complete input.
 */
[[nodiscard]] bool encrypt_cbc(std::span<const std::byte, state::kAesKeySize> key,
                               std::span<const std::byte, kPlaintextSize> plaintext,
                               std::span<const std::byte, kIvSize> iv,
                               std::span<std::byte, kCiphertextSize> ciphertext) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE symmetricKey = nullptr;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    if (BCryptSetProperty(algorithm,
                          BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC),
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
        std::array<std::byte, kIvSize> mutableIv;
        for (std::size_t index = 0; index < iv.size(); ++index) {
            mutableIv[index] = iv[index];
        }
        ULONG encrypted = 0;
        success = BCryptEncrypt(symmetricKey,
                                reinterpret_cast<PUCHAR>(const_cast<std::byte*>(plaintext.data())),
                                static_cast<ULONG>(plaintext.size()),
                                nullptr,
                                reinterpret_cast<PUCHAR>(mutableIv.data()),
                                static_cast<ULONG>(mutableIv.size()),
                                reinterpret_cast<PUCHAR>(ciphertext.data()),
                                static_cast<ULONG>(ciphertext.size()),
                                &encrypted,
                                0)
                      >= 0
                  && encrypted == ciphertext.size();
        SecureZeroMemory(mutableIv.data(), mutableIv.size());
    }
    if (symmetricKey != nullptr) {
        BCryptDestroyKey(symmetricKey);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

/**
 * Computes the service-26 authentication code with Windows HMAC-SHA256.
 * @param key Authentication key issued by SignOn.
 * @param input Envelope prefix authenticated by the protocol.
 * @param output Receives the complete SHA-256 MAC.
 * @return True when Windows CNG hashes the complete input.
 */
[[nodiscard]] bool hmac_sha256(std::span<const std::byte, state::kAesKeySize> key,
                               std::span<const std::byte, kAuthenticatedSize> input,
                               std::span<std::byte, kMacSize> output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)
        < 0) {
        return false;
    }
    if (BCryptCreateHash(algorithm,
                         &hash,
                         nullptr,
                         0,
                         reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())),
                         static_cast<ULONG>(key.size()),
                         0)
        >= 0) {
        success = BCryptHashData(hash,
                                 reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                 static_cast<ULONG>(input.size()),
                                 0)
                      >= 0
                  && BCryptFinishHash(hash,
                                      reinterpret_cast<PUCHAR>(output.data()),
                                      static_cast<ULONG>(output.size()),
                                      0)
                         >= 0;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

} // namespace

/** Encodes the authenticated service-26 key and nonce envelope. */
bool encode_server_hello(const state::SignOnState& signOn,
                         const state::BapState& bap,
                         std::span<std::byte> output,
                         std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kServerHelloEnvelopeSize) {
        return false;
    }
    std::array<std::byte, kPlaintextSize> plaintext{};
    for (std::size_t index = 0; index < bap.nonce.size(); ++index) {
        plaintext[index] = bap.nonce[index];
    }
    for (std::size_t index = 0; index < bap.sessionKey.size(); ++index) {
        plaintext[bap.nonce.size() + index] = bap.sessionKey[index];
    }
    for (std::size_t index = bap.nonce.size() + bap.sessionKey.size(); index < plaintext.size();
         ++index) {
        plaintext[index] = kPaddingByte;
    }
    encoding::write_u32_be(output.subspan<kEnvelopeLengthOffset, encoding::kU32Size>(),
                           kEnvelopePayloadSize);
    for (std::size_t index = 0; index < bap.envelopeIv.size(); ++index) {
        output[kEnvelopeIvOffset + index] = bap.envelopeIv[index];
    }
    const bool encrypted =
        encrypt_cbc(signOn.encryptionKey,
                    plaintext,
                    bap.envelopeIv,
                    output.subspan<kEnvelopeCiphertextOffset, kCiphertextSize>());
    SecureZeroMemory(plaintext.data(), plaintext.size());
    if (!encrypted
        || !hmac_sha256(signOn.authenticationKey,
                        output.first<kAuthenticatedSize>(),
                        output.subspan<kAuthenticatedSize, kMacSize>())) {
        return false;
    }
    written = kServerHelloEnvelopeSize;
    return true;
}

} // namespace sunrise::middleware::secure_channel
