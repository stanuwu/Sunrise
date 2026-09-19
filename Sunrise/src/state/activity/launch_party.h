#pragma once

#include <array>
#include <cstdint>

#include "membership/definition.h"

namespace sunrise::state::activity {
/** An authenticated account whose own native allocation has committed for this generation. */
struct LaunchOwner final {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    bool operator==(const LaunchOwner&) const noexcept = default;
};

/** Allocation evidence from the native launch request; it neither admits nor readies members. */
struct LaunchParty final {
    std::array<membership::Identity, 32> identities{};
    std::uint64_t publisherAccount{};
    std::uint64_t publisherCharacter{};
    std::uint8_t count{};
    bool operator==(const LaunchParty&) const noexcept = default;
};

/** Public selection revisions retained by a prepared allocation. */
struct LaunchPartyGuard final {
    std::array<std::uint64_t, 32> generations{};
    std::uint64_t publisherGeneration{};
    bool operator==(const LaunchPartyGuard&) const noexcept = default;
};
} // namespace sunrise::state::activity
