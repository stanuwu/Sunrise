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

#pragma pack(push, 1)
/** What both ends hash with the shared point. */
struct TranscriptWire final {
    middleware::crypto::sha256::Digest joinSecret;
    PublicKey guestPublic;
    PublicKey hostPublic;
};
struct AcceptWire final {
    std::array<char, 4> magic;
    std::uint8_t version;
    PublicKey hostPublic;
    Confirm confirm;
};
struct MaterialWire final {
    std::uint8_t version;
    std::array<std::byte, kAesKeySize> encryptionKey;
    std::array<std::byte, kAesKeySize> authenticationKey;
};
#pragma pack(pop)
static_assert(sizeof(AcceptWire) == kAcceptSize && sizeof(MaterialWire) == kMaterialSize);

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
    const TranscriptWire transcript{joinSecret, guestPublic, hostPublic};
    std::array<std::byte, ecdh::kSecretSize> secret{};
    hmac::Digest key{}, nonce{}, tag{};
    const bool done = own.agree(peer, std::as_bytes(std::span(&transcript, 1)), secret)
                      && expand(secret, kKeyLabel, key) && expand(secret, kNonceLabel, nonce)
                      && expand(secret, kConfirmLabel, tag);
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
    const AcceptWire wire{kAcceptMagic, kWireVersion, hostPublic, confirm};
    std::memcpy(output.data(), &wire, sizeof wire);
}

bool decode_accept(std::span<const std::byte> body,
                   PublicKey& hostPublic,
                   Confirm& confirm) noexcept {
    if (body.size() != sizeof(AcceptWire)) {
        return false;
    }
    AcceptWire wire{};
    std::memcpy(&wire, body.data(), sizeof wire);
    if (wire.magic != kAcceptMagic || wire.version != kWireVersion) {
        return false;
    }
    hostPublic = wire.hostPublic;
    confirm = wire.confirm;
    return true;
}

void encode_material(const SignOnState& signOn,
                     std::span<std::byte, kMaterialSize> output) noexcept {
    const MaterialWire wire{kWireVersion, signOn.encryptionKey, signOn.authenticationKey};
    std::memcpy(output.data(), &wire, sizeof wire);
}

bool decode_material(std::span<const std::byte> body, SignOnState& output) noexcept {
    if (body.size() != sizeof(MaterialWire)) {
        return false;
    }
    MaterialWire wire{};
    std::memcpy(&wire, body.data(), sizeof wire);
    if (wire.version != kWireVersion) {
        return false;
    }
    output.encryptionKey = wire.encryptionKey;
    output.authenticationKey = wire.authenticationKey;
    SecureZeroMemory(&wire, sizeof wire);
    return true;
}

} // namespace sunrise::state::coop::channel_key