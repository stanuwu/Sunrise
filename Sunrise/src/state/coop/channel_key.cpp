#include "channel_key.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "../../middleware/crypto/hmac.h"

namespace sunrise::state::coop::channel_key {
namespace {

namespace hmac = middleware::crypto::hmac;

/** Leads the accepting hello reply. */
constexpr std::array<char, 4> kAcceptMagic{'S', 'R', 'C', 'K'};
/** Raised whenever the handshake or a derivation label changes. */
constexpr std::uint8_t kWireVersion = 1;
/** Labels that split one handshake secret into the session key and the base nonce. */
constexpr std::string_view kKeyLabel = "sunrise-coop-session-key";
constexpr std::string_view kNonceLabel = "sunrise-coop-session-nonce";
constexpr std::string_view kConfirmLabel = "sunrise-coop-host-confirm";

/** @return HMAC-SHA256 of `label` under `secret`, or false when Windows refused. */
[[nodiscard]] bool
expand(std::span<const std::byte> secret, std::string_view label, hmac::Digest& output) noexcept {
    return hmac::authenticate(
               hmac::Algorithm::sha256, secret, std::as_bytes(std::span(label)), {}, output)
           && output.size >= kAesKeySize && output.size >= kBapNonceSize;
}

} // namespace

bool derive(const ecdh::KeyPair& own,
            const PublicKey& peer,
            const middleware::crypto::sha256::Digest& joinSecret,
            const PublicKey& guestPublic,
            const PublicKey& hostPublic,
            BapState& output,
            Confirm& confirm) noexcept {
    output = {};
    confirm = {};
    std::array<std::byte, 32 + 2 * ecdh::kPublicKeySize> transcript{};
    std::memcpy(transcript.data(), joinSecret.data(), joinSecret.size());
    std::memcpy(transcript.data() + 32, guestPublic.data(), guestPublic.size());
    std::memcpy(transcript.data() + 32 + guestPublic.size(), hostPublic.data(), hostPublic.size());
    std::array<std::byte, ecdh::kSecretSize> secret{};
    hmac::Digest key{}, nonce{}, tag{};
    const bool done = own.agree(peer, transcript, secret) && expand(secret, kKeyLabel, key)
                      && expand(secret, kNonceLabel, nonce) && expand(secret, kConfirmLabel, tag);
    if (done) {
        std::copy_n(key.bytes.begin(), output.sessionKey.size(), output.sessionKey.begin());
        std::copy_n(nonce.bytes.begin(), output.nonce.size(), output.nonce.begin());
        std::copy_n(tag.bytes.begin(), confirm.size(), confirm.begin());
    }
    SecureZeroMemory(secret.data(), secret.size());
    SecureZeroMemory(key.bytes.data(), key.bytes.size());
    SecureZeroMemory(nonce.bytes.data(), nonce.bytes.size());
    return done;
}

void encode_accept(const PublicKey& hostPublic,
                   const Confirm& confirm,
                   std::span<std::byte, kAcceptSize> output) noexcept {
    constexpr std::size_t kKeyAt = 4 + 1;
    std::memcpy(output.data(), kAcceptMagic.data(), kAcceptMagic.size());
    output[kAcceptMagic.size()] = static_cast<std::byte>(kWireVersion);
    std::memcpy(output.data() + kKeyAt, hostPublic.data(), hostPublic.size());
    std::memcpy(output.data() + kKeyAt + hostPublic.size(), confirm.data(), confirm.size());
}

bool decode_accept(std::span<const std::byte> body,
                   PublicKey& hostPublic,
                   Confirm& confirm) noexcept {
    constexpr std::size_t kKeyAt = 4 + 1;
    if (body.size() != kAcceptSize
        || std::memcmp(body.data(), kAcceptMagic.data(), kAcceptMagic.size()) != 0
        || body[kAcceptMagic.size()] != static_cast<std::byte>(kWireVersion)) {
        return false;
    }
    std::memcpy(hostPublic.data(), body.data() + kKeyAt, hostPublic.size());
    std::memcpy(confirm.data(), body.data() + kKeyAt + hostPublic.size(), confirm.size());
    return true;
}

void encode_material(const SignOnState& signOn,
                     std::span<std::byte, kMaterialSize> output) noexcept {
    output[0] = static_cast<std::byte>(kWireVersion);
    std::memcpy(output.data() + 1, signOn.encryptionKey.data(), kAesKeySize);
    std::memcpy(output.data() + 1 + kAesKeySize, signOn.authenticationKey.data(), kAesKeySize);
}

bool decode_material(std::span<const std::byte> body, SignOnState& output) noexcept {
    if (body.size() != kMaterialSize || body[0] != static_cast<std::byte>(kWireVersion)) {
        return false;
    }
    std::memcpy(output.encryptionKey.data(), body.data() + 1, kAesKeySize);
    std::memcpy(output.authenticationKey.data(), body.data() + 1 + kAesKeySize, kAesKeySize);
    return true;
}

} // namespace sunrise::state::coop::channel_key