#pragma once

#include <array>
#include <cstdint>

namespace sunrise::state::gameplay::external {

/** Protected carrier used by one peer generation. */
enum class CarrierKind : std::uint8_t {
    engineAssociation,
    dtls,
};

/** Identity of one established protected carrier. */
struct CarrierKey {
    CarrierKind kind{};
    std::uint64_t generation{};
};

/** Exact peer, view, activity, and channel tuple for one observation. */
struct ExternalShadowKey {
    CarrierKey carrier{};
    std::uint64_t authenticatedMemberId{};
    std::uint64_t peerGeneration{};
    std::uint64_t channelGeneration{};
    std::uint64_t groupSessionId{};
    std::uint64_t viewGeneration{};
    std::uint64_t activitySessionId{};
    std::uint32_t remoteConnectionSequence{};
    std::uint32_t localConnectionSequence{};
};

/** Receive-only common state retained for one exact context. */
struct ExternalShadow {
    ExternalShadowKey key{};
    std::array<std::uint64_t, 2> patchEpoch{};
    std::uint8_t reconciliationGeneration{};
    std::uint8_t defaultBubble{};
    bool defaultBubblePresent{};
    bool channel3TrailingList{};
    bool occupied{};
};

} // namespace sunrise::state::gameplay::external
