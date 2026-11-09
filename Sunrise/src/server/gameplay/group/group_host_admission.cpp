#include "group_host_admission.h"

#include <algorithm>
#include <atomic>

#include "../../../middleware/crypto/lookup3.h"

namespace sunrise::server::gameplay::group::admission {
/** Stamps `Admitted::lastUse`. It only has to order the records, so it never has to be a clock. */
std::atomic<std::uint64_t> g_admitClock{0};
SRWLOCK g_admittedLock{SRWLOCK_INIT};
// Native joins claim endpoint-specific rows; departures release only the owning row.
std::array<Admitted, kAdmittedCapacity> g_admitted{};

bool view_owner(const state::gameplay::Endpoint& peer,
                wire::PlayerBlockSoids& output,
                std::uint64_t participantGroup) noexcept {
    output = {};
    wire::PlayerBlockSoids owner{};
    bool ambiguous{};
    AcquireSRWLockShared(&g_admittedLock);
    for (const auto& row : g_admitted) {
        const bool sibling = participantGroup && row.sessionId == participantGroup
                             && row.endpoint.address == peer.address
                             && row.endpoint.port == peer.port;
        if (!row.occupied || !row.hasPlayer || !row.playerSoids.present
            || (row.endpoint != peer && !sibling)) {
            continue;
        }
        if (owner.present
            && (owner.accountSoid != row.playerSoids.accountSoid
                || owner.characterSoid != row.playerSoids.characterSoid)) {
            ambiguous = true;
        }
        owner = row.playerSoids;
    }
    ReleaseSRWLockShared(&g_admittedLock);
    if (ambiguous) {
        return false;
    }
    output = owner;
    return true;
}

bool accounts_coresident(std::uint64_t first, std::uint64_t second) noexcept {
    if (!first || !second || first == second) {
        return false;
    }
    bool found = false;
    AcquireSRWLockShared(&g_admittedLock);
    for (const auto& a : g_admitted) {
        if (!a.occupied || !a.sessionId || !a.hasPlayer || !a.playerSoids.present
            || a.playerSoids.accountSoid != first) {
            continue;
        }
        for (const auto& b : g_admitted) {
            if (b.occupied && b.sessionId == a.sessionId && b.hasPlayer && b.playerSoids.present
                && b.playerSoids.accountSoid == second) {
                found = true;
            }
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return found;
}

[[nodiscard]] Admitted* find_admitted(std::uint64_t sessionId) noexcept {
    Admitted* found = nullptr;
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &entry;
        }
    }
    return found;
}

Admitted* find(const state::gameplay::Endpoint& peer, std::uint64_t sessionId) noexcept {
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId && entry.endpoint == peer) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept {
    if (sessionId == 0 || peer.address == 0 || peer.port == 0) {
        return nullptr;
    }
    Admitted* found = find(peer, sessionId);
    if (found == nullptr) {
        for (Admitted& entry : g_admitted) {
            if (!entry.occupied) {
                entry = {};
                entry.occupied = true;
                entry.sessionId = sessionId;
                found = &entry;
                break;
            }
        }
    }
    if (found != nullptr) {
        found->endpoint = peer;
        found->lastUse = g_admitClock.fetch_add(1) + 1;
    }
    return found;
}

[[nodiscard]] Admitted* find_owned(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept {
    Admitted* const found = find(peer, sessionId);
    if (found == nullptr) {
        return nullptr;
    }
    found->lastUse = g_admitClock.fetch_add(1) + 1;
    return found;
}

[[nodiscard]] bool owned_elsewhere(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_admittedLock);
    bool conflict = false;
    if (find(peer, sessionId) == nullptr) {
        for (const auto& entry : g_admitted) {
            conflict = conflict || (entry.occupied && entry.sessionId == sessionId);
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return conflict;
}

[[nodiscard]] std::uint8_t session_player_count(std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_admittedLock);
    std::uint8_t count = 0;
    for (const auto& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId && entry.hasPlayer) {
            ++count;
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return count;
}

void mark_stale(std::uint64_t sessionId) noexcept {
    for (auto& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry.rosterStale = true;
            if (entry.joinComplete) {
                entry.parameterOwed = true;
            }
        }
    }
}

void refresh_endpoint(const state::gameplay::Endpoint& peer) noexcept {
    for (auto& entry : g_admitted) {
        if (!entry.occupied || entry.endpoint != peer) {
            continue;
        }
        entry.publication = {};
        mark_stale(entry.sessionId);
    }
}

std::uint32_t member_mask(std::uint64_t sessionId) noexcept {
    std::uint32_t count = 1;
    for (const auto& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            ++count;
        }
    }
    return (1U << count) - 1U;
}

bool visible_member(const Admitted& recipient, const Admitted& peer) noexcept {
    return recipient.occupied && peer.occupied && recipient.sessionId == peer.sessionId
           && residency::member(&recipient == &peer,
                                recipient.presence.sessionRegion,
                                peer.presence.pendingRegion,
                                peer.presence.currentRegion);
}

bool visible_player(const Admitted& recipient, const Admitted& peer) noexcept {
    return peer.hasPlayer && visible_member(recipient, peer)
           && residency::player(&recipient == &peer,
                                recipient.playerSoids.accountSoid,
                                peer.playerSoids.accountSoid,
                                recipient.presence.sessionRegion,
                                peer.presence.pendingRegion,
                                peer.presence.currentRegion,
                                recipient.presence.connected(peer.playerSoids.accountSoid));
}

std::uint32_t member_mask(const Admitted& recipient) noexcept {
    std::uint32_t count = 1;
    for (const auto& peer : g_admitted) {
        if (visible_member(recipient, peer)) {
            ++count;
        }
    }
    return (1U << count) - 1U;
}

std::uint8_t player_count(const Admitted& recipient) noexcept {
    std::uint8_t count = 0;
    for (const auto& peer : g_admitted) {
        if (visible_player(recipient, peer)) {
            ++count;
        }
    }
    return count;
}

bool set_player(Admitted& record, const wire::PlayerAddRequest& request) noexcept {
    if (!record.occupied || record.sessionId != request.sessionId || !request.playerId
        || request.kind > kMaximumPlayerKind || !wire::valid_native_player_profile(request.profile)
        || (request.soids.present
            && (!request.soids.accountSoid || !request.soids.characterSoid))) {
        return false;
    }
    std::uint32_t occupiedSlots = 0, nextSequence = 0;
    for (const auto& entry : g_admitted) {
        if (!entry.occupied || &entry == &record || entry.sessionId != record.sessionId
            || !entry.hasPlayer) {
            continue;
        }
        if (entry.playerId == request.playerId
            || (entry.playerSoids.present && request.soids.present
                && entry.playerSoids.accountSoid == request.soids.accountSoid)) {
            return false;
        }
        occupiedSlots |= 1U << entry.playerSlot;
        nextSequence = std::max(nextSequence, entry.playerAddSequence + 1);
    }
    if (!record.hasPlayer) {
        std::uint32_t slot = 0;
        while (slot < kPlayerSlotCount && (occupiedSlots & (1U << slot))) {
            ++slot;
        }
        if (slot == kPlayerSlotCount) {
            return false;
        }
        record.playerSlot = slot;
        record.playerAddSequence = nextSequence & kPlayerAddSequenceMask;
    }
    if (!record.hasPlayer || record.playerId != request.playerId
        || record.playerSoids.present != request.soids.present
        || record.playerSoids.accountSoid != request.soids.accountSoid
        || record.playerSoids.characterSoid != request.soids.characterSoid) {
        record.presence = {};
        record.presencePending = true;
    }
    record.hasPlayer = true;
    record.playerId = request.playerId;
    record.playerKind = request.kind;
    record.playerProfileValue = request.sequence;
    record.playerSoids = request.soids;
    record.playerProfile = request.profile;
    mark_stale(record.sessionId);
    return true;
}

bool update_player(Admitted& record, const wire::PlayerPropertiesRequest& request) noexcept {
    if (!record.occupied || !record.hasPlayer || record.sessionId != request.sessionId
        || request.kind > kMaximumPlayerKind || !wire::valid_native_player_profile(request.profile)
        || !request.profile.hasTail) {
        return false;
    }
    // A sparse update cannot transfer a player to another account or character. That requires
    // the native remove/add lifecycle, which clears both the profile and its presence sample.
    const auto& owner = request.profile.soids;
    if (owner.present) {
        if (!owner.accountSoid || !owner.characterSoid) {
            return false;
        }
        if (record.playerSoids.present
            && (owner.accountSoid != record.playerSoids.accountSoid
                || owner.characterSoid != record.playerSoids.characterSoid)) {
            return false;
        }
        for (const auto& other : g_admitted) {
            if (&other != &record && other.occupied && other.hasPlayer
                && other.sessionId == record.sessionId && other.playerSoids.present
                && other.playerSoids.accountSoid == owner.accountSoid) {
                return false;
            }
        }
    }
    if (request.hasBaselineChecksum) {
        wire::NativePlayerProfileState baseline;
        wire::build_native_player_profile_state(record.playerProfile, baseline);
        // Native 17AF360 still merges on a baseline disagreement. Republish the complete
        // retained state to this endpoint, so a lost baseline does not discard the update.
        if (!wire::complete_native_player_profile(record.playerProfile)
            // The seed native 17AF360 itself hashes that image with.
            || middleware::crypto::lookup3::hash_bytes(baseline, kBaselineChecksumSeed)
                   != request.baselineChecksum) {
            record.publication = {};
        }
    }
    wire::merge_native_player_profile(record.playerProfile, request.profile);
    if (owner.present && !record.playerSoids.present) {
        record.playerSoids = owner;
        record.presence = {};
        record.presencePending = true;
    }
    record.playerKind = request.kind;
    record.playerProfileValue = request.sequence;
    mark_stale(record.sessionId);
    return true;
}

bool clear_player(Admitted& record) noexcept {
    if (!record.occupied || !record.hasPlayer) {
        return false;
    }
    record.hasPlayer = false;
    record.playerId = 0;
    record.playerKind = 0;
    record.playerProfileValue = 0;
    record.playerSoids = {};
    record.playerProfile = {};
    record.presence = {};
    record.presencePending = false;
    record.playerSlot = record.playerAddSequence = 0;
    mark_stale(record.sessionId);
    return true;
}

bool depart(const state::gameplay::Endpoint& peer, std::uint64_t sessionId) noexcept {
    auto* record = find(peer, sessionId);
    if (record == nullptr) {
        return false;
    }
    *record = {};
    mark_stale(sessionId);
    return true;
}

bool retire_excess(Admitted& retired) noexcept {
    retired = {};
    for (auto& candidate : g_admitted) {
        if (!candidate.occupied) {
            continue;
        }
        std::size_t count = 0;
        Admitted* oldest = &candidate;
        for (auto& entry : g_admitted) {
            if (!entry.occupied || entry.endpoint != candidate.endpoint) {
                continue;
            }
            ++count;
            if (entry.lastUse < oldest->lastUse) {
                oldest = &entry;
            }
        }
        if (count <= kPublicSessionCapacity) {
            continue;
        }
        retired = *oldest;
        *oldest = {};
        mark_stale(retired.sessionId);
        return true;
    }
    return false;
}

} // namespace sunrise::server::gameplay::group::admission
