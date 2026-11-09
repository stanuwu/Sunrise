#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/crypto/ecdh_p256.h"
#include "../../middleware/crypto/sha256.h"
#include "../runtime/state.h"

namespace sunrise::state::coop::channel_key {

namespace ecdh = middleware::crypto::ecdh_p256;
using PublicKey = std::array<std::byte, ecdh::kPublicKeySize>;

/** Sunrise-only request carrying a guest's sign-on keys to the host, on its encrypted link. */
inline constexpr std::uint16_t kMaterialService = 0x8005;
/** Bytes of the tag that proves the host derived the same keys. */
inline constexpr std::size_t kConfirmSize = 16;
using Confirm = std::array<std::byte, kConfirmSize>;
/** Bytes of the host's accepting hello reply: magic, version, host public key, confirm tag. */
inline constexpr std::size_t kAcceptSize = 4 + 1 + ecdh::kPublicKeySize + kConfirmSize;
/** Bytes of one key report: version, encryption key, authentication key. */
inline constexpr std::size_t kMaterialSize = 1 + 2 * kAesKeySize;

/**
 * Derives one Sunrise link's session key, base nonce and confirm tag. Both ends pass the same
 * transcript and the join code's secret hash, so a peer without the code derives other keys.
 * @param own This end's ephemeral pair.
 * @param peer The other end's public key.
 * @param joinSecret Secret hash of the join code; all zero when none is set.
 * @return False when the agreement failed.
 */
[[nodiscard]] bool derive(const ecdh::KeyPair& own,
                          const PublicKey& peer,
                          const middleware::crypto::sha256::Digest& joinSecret,
                          const PublicKey& guestPublic,
                          const PublicKey& hostPublic,
                          BapState& output,
                          Confirm& confirm) noexcept;

/** Writes the host's accepting hello reply. */
void encode_accept(const PublicKey& hostPublic,
                   const Confirm& confirm,
                   std::span<std::byte, kAcceptSize> output) noexcept;
/** @return True when `body` is exactly one accepting reply. */
[[nodiscard]] bool
decode_accept(std::span<const std::byte> body, PublicKey& hostPublic, Confirm& confirm) noexcept;

/** Writes one key report from this install's sign-on keys. */
void encode_material(const SignOnState& signOn,
                     std::span<std::byte, kMaterialSize> output) noexcept;
/** @return True when `body` is exactly one key report; only the two keys of `output` are set. */
[[nodiscard]] bool decode_material(std::span<const std::byte> body, SignOnState& output) noexcept;

} // namespace sunrise::state::coop::channel_key