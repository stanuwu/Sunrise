#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../entity_slots/member_leases.h"
#include "../membership/definition.h"

namespace sunrise::state::activity::reservations {

/** Thirty human peers leave the native 32-slot table room for its owner and Bubble Host. */
inline constexpr std::size_t kPeerCapacity = 30;

/**
 * A native requested identity. It confers neither a client join nor readiness nor entity
 * leases.
 */
struct Roster final {
    /** A surviving client may reserve the departed original owner at native slot zero. */
    membership::Identity primary{};
    std::array<membership::Identity, kPeerCapacity> peers{};
    bool operator==(const Roster&) const noexcept = default;
};

/** Which reservation operation a prepared mutation represents; `none` marks an unprepared plan. */
enum class Kind : std::uint8_t { none, admit, release };

/** Why a native retract took nothing back. Neither refusal changes state. */
enum class ReleaseRefusal : std::uint8_t { none, hostRow, leaseHeld };

/** Complete bounded after-image, committed only with the original session and record revisions. */
struct PendingMutation final {
    Roster after{};
    Roster afterGuard{};
    membership::Identity primaryIdentity{};
    std::array<membership::Identity, entity_slots::kMemberLeaseRowCount> joinedIdentities{};
    std::array<std::uint32_t, kPeerCapacity> accountGenerations{};
    std::uint32_t primaryAccountGeneration{};
    std::uint64_t sessionId{};
    std::uint64_t publisherAccount{};
    std::uint64_t expectedStateRevision{};
    std::uint64_t expectedRecordRevision{};
    std::uint64_t expectedCreatedRevision{};
    std::uint64_t publisherMemberKey{};
    std::uint64_t releasedMemberKey{};
    std::size_t publisherRow{entity_slots::kMemberLeaseRowCount};
    std::size_t releasedMemberRow{entity_slots::kMemberLeaseRowCount};
    std::uint32_t membershipRevision{};
    std::uint32_t peerTableEpoch{};
    /** Rows still holding entity-slot leases, read under one lock. */
    std::uint32_t leasedRows{};
    std::size_t targetSlot{};
    Kind kind{};
    bool changed{};
    bool prepared{};
};
} // namespace sunrise::state::activity::reservations
