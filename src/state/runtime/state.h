#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../account/account_state.h"
#include "../activity/definition.h"
#include "../investment/investment.h"
#include "../matchmaking/definition.h"

namespace sunrise::state {

/** AES-128 protocol keys are 16 bytes. */
inline constexpr std::size_t kAesKeySize = 16;
/** SignOn session tokens are 32 opaque bytes. */
inline constexpr std::size_t kSessionTokenSize = 32;
/** The native bootstrap content-id token is 16 bytes. */
inline constexpr std::size_t kBootstrapTokenSize = 16;
/** BAP AES-GCM uses the standard 12-byte nonce size. */
inline constexpr std::size_t kBapNonceSize = 12;
/** Service-26 AES-CBC uses one 16-byte initialization vector. */
inline constexpr std::size_t kEnvelopeIvSize = 16;

/** Secrets and relay fields returned by the SignOn route. */
struct SignOnState {
    std::array<std::byte, kAesKeySize> encryptionKey{};
    std::array<std::byte, kAesKeySize> authenticationKey{};
    std::array<std::byte, kSessionTokenSize> sessionToken{};
    std::uint32_t relayAddress{};
    std::uint16_t relayPort{};
    std::uint32_t tokenLifetimeSeconds{};
    /** Read from the installed client at activation. Never bundled, saved, or logged. */
    std::array<std::byte, kBootstrapTokenSize> bootstrapToken{};
    bool bootstrapTokenPresent{};
};

/** Session key and initial nonce material returned by service 26. */
struct BapState {
    std::array<std::byte, kBapNonceSize> nonce{};
    std::array<std::byte, kAesKeySize> sessionKey{};
    std::array<std::byte, kEnvelopeIvSize> envelopeIv{};
};

/** Process-local state shared through the State layer. */
struct State {
    SignOnState signOn;
    BapState bap;
    AccountState account;
    activity::ActivityState activity;
    InvestmentState investment;
    matchmaking::MatchmakingState matchmaking;
};

} // namespace sunrise::state
