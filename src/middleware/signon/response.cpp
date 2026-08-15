#include "response.h"

#include <Windows.h>

#include <array>
#include <span>
#include <string_view>

#include "config/signon_config_blob.h"
#include "extended/signon_extended_fields.h"
#include "internal.h"
#include "ownership/signon_ownership_encoder.h"

namespace sunrise::middleware::signon {
namespace {

/** Scratch size covers every required and optional success field. */
constexpr std::size_t kSuccessBufferSize = 320;
/** SignOn success field 1 requires 16 reserved zero bytes. */
constexpr std::size_t kReservedSize = 16;
/** SignOn success protobuf field numbers. */
constexpr unsigned kReservedField = 1;
constexpr unsigned kEncryptionKeyField = 2;
constexpr unsigned kAuthenticationKeyField = 3;
constexpr unsigned kSessionTokenField = 4;
constexpr unsigned kExpiryField = 5;
constexpr unsigned kRelayAddressField = 6;
constexpr unsigned kRelayPortField = 7;
/** SignOn response protobuf field numbers. */
constexpr unsigned kResponseErrorField = 1;
constexpr unsigned kResponseSuccessField = 2;
constexpr unsigned kCommonInfoField = 4;
constexpr unsigned kSiloIdField = 5;
constexpr unsigned kContentBuildVersionField = 6;
constexpr unsigned kCapabilityFlagsField = 8;
constexpr unsigned kOwnedEntitlementsField = 10;
constexpr unsigned kServerTimeField = 12;
constexpr unsigned kEnvironmentField = 13;
constexpr unsigned kExternalAddressField = 14;
/** CommonInfo sub-message field numbers. */
constexpr unsigned kCommonInfoIdField = 1;
constexpr unsigned kCommonInfoSiloField = 2;
constexpr unsigned kCommonInfoConfigBlobField = 3;
/** SignOn error value used for a successful response. */
constexpr std::uint64_t kNoError = 0;
/**
 * Mock SignOn identity the client accepts. Not secret, not derived from the package.
 * CommonInfo field 3 carries the bootstrap token read from the installed client.
 */
constexpr std::uint64_t kCommonId = 1;
constexpr std::uint32_t kSiloId = 1;
constexpr std::uint32_t kCapabilityFlags = 0;
constexpr std::string_view kEnvironment = "live";
constexpr std::string_view kContentBuildVersion = "d2legacy";
/** Scratch size covers the CommonInfo sub-message and its blob. */
constexpr std::size_t kCommonInfoBufferSize = 64;

} // namespace

/** Encodes the SignOn success protobuf into fixed caller storage. */
bool encode_success(const state::SignOnState& state,
                    const state::entitlements::Table& entitlements,
                    std::uint64_t expirySeconds,
                    std::uint64_t serverTimeSeconds,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    std::array<std::byte, kSuccessBufferSize> successBuffer{};
    Writer success(successBuffer);
    /** SignOn field 1 is a fixed zero-filled reserved value. */
    constexpr std::array<std::byte, kReservedSize> reserved{};
    const bool successEncoded = success.bytes(kReservedField, reserved)
                                && success.bytes(kEncryptionKeyField, state.encryptionKey)
                                && success.bytes(kAuthenticationKeyField, state.authenticationKey)
                                && success.bytes(kSessionTokenField, state.sessionToken)
                                && success.varint(kExpiryField, expirySeconds)
                                && success.varint(kRelayAddressField, state.relayAddress)
                                && success.varint(kRelayPortField, state.relayPort)
                                && extended::append(success, state.relayAddress);
    if (!successEncoded) {
        SecureZeroMemory(successBuffer.data(), successBuffer.size());
        return false;
    }

    std::array<std::byte, kCommonInfoBufferSize> commonInfoBuffer{};
    Writer commonInfo(commonInfoBuffer);
    std::array<std::byte, config::kBlobSize> configBlob{};
    // Without the token the client cannot derive its content id, and package validation fails.
    const bool blobReady = state.bootstrapTokenPresent;
    if (blobReady) {
        config::build(state.bootstrapToken, configBlob);
    }
    const bool commonInfoEncoded =
        commonInfo.varint(kCommonInfoIdField, kCommonId)
        && commonInfo.varint(kCommonInfoSiloField, kSiloId)
        && (!blobReady || commonInfo.bytes(kCommonInfoConfigBlobField, configBlob));

    std::array<std::byte, ownership::kBufferSize> ownedBuffer{};
    std::size_t ownedSize = 0;
    const bool ownedEncoded = ownership::encode(entitlements, ownedBuffer, ownedSize);

    Writer response(output);
    const bool responseEncoded =
        commonInfoEncoded && ownedEncoded && response.varint(kResponseErrorField, kNoError)
        && response.bytes(kResponseSuccessField, std::span(successBuffer).first(success.size()))
        && response.bytes(kCommonInfoField, std::span(commonInfoBuffer).first(commonInfo.size()))
        && response.varint(kSiloIdField, kSiloId)
        && response.bytes(kContentBuildVersionField, text_bytes(kContentBuildVersion))
        && response.varint(kCapabilityFlagsField, kCapabilityFlags)
        && response.bytes(kOwnedEntitlementsField, std::span(ownedBuffer).first(ownedSize))
        && response.varint(kServerTimeField, serverTimeSeconds)
        && response.bytes(kEnvironmentField, text_bytes(kEnvironment))
        && response.varint(kExternalAddressField, state.relayAddress);
    SecureZeroMemory(successBuffer.data(), successBuffer.size());
    SecureZeroMemory(commonInfoBuffer.data(), commonInfoBuffer.size());
    if (!responseEncoded) {
        written = 0;
        return false;
    }
    written = response.size();
    return true;
}

} // namespace sunrise::middleware::signon
