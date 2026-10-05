#pragma once

#include <array>

#include "definition.h"

namespace sunrise::state::activity::membership {
/** A peer row is either a reservation or the reports received from that exact joined client. */
struct ObservedMember final {
    Identity identity{};
    RegionState currentLeg{};
    RegionState pendingLeg{};
    std::uint8_t slot{};
    std::uint8_t transitionToken{};
    std::uint8_t teleportToken{};
    bool currentReported{};
    bool pendingReported{};
    bool hasTransitionToken{};
    bool hasTeleportToken{};
    bool joined{};
    bool present{};
};

/** Recipient-relative view with stable slots inside one activity generation. */
struct MemberDirectory final {
    /** Thirty peers: the native 32-slot table less its owner and Bubble Host. */
    std::array<ObservedMember, 30> peers{};
    std::uint8_t localSlot{};
    bool valid{};
};
} // namespace sunrise::state::activity::membership
