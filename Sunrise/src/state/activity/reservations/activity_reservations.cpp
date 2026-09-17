#include <Windows.h>

#include <algorithm>

#include "../../account/account_context.h"
#include "../../account/public_profiles.h"
#include "../../runtime/storage/internal.h"
#include "../member_context.h"
#include "../member_mutation.h"
#include "../transactions/internal.h"
#include "runtime.h"

namespace sunrise::state::activity::reservations {
namespace {
namespace profiles = account::profiles;

bool known(const membership::Identity& identity, std::uint32_t& generation) noexcept {
    generation = 0;
    AccountHandle handle{};
    // smallOpaque is ten bits at bias one, so -1 is its unset value and 1022 its largest.
    if (identity.memberKey == 0 || identity.accountSoid == 0 || identity.opaqueSoid == 0
        || identity.smallOpaque < -1 || identity.smallOpaque > 1022
        || !profiles::find(identity.accountSoid, handle)
        || profiles::selected_character(handle) != identity.opaqueSoid) {
        return false;
    }
    generation = profiles::generation(handle);
    return generation != 0;
}

/** Captures only an existing, joined session owned by the authenticated requester. */
bool prepare_base(std::uint64_t sessionId, PendingMutation& prepared) noexcept {
    const auto publisher = account_primary_soid(bound_account());
    if (publisher == 0 || sessionId == 0) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const auto& activity = runtime::storage::g_state.activity;
    const auto slot = transactions::find_session(activity, sessionId);
    bool ready = slot != kInvalidSessionSlot && activity.stateRevision != kMaximumRevision;
    if (ready) {
        const auto& record = activity.sessions[slot];
        const auto context = member_context();
        prepared.publisherRow =
            record.sharedMembers
                ? member_row(
                      record, context.sessionId == sessionId ? context.memberKey : 0, publisher)
                : 0;
        const auto* member = member_state(record, prepared.publisherRow);
        ready = record.joined && record.joinedRevision != 0 && member && member->hasIdentity
                && member->identity.accountSoid == publisher && member->identity.memberKey != 0;
        if (ready) {
            prepared.sessionId = sessionId;
            prepared.publisherAccount = publisher;
            prepared.publisherMemberKey = member->identity.memberKey;
            prepared.primaryIdentity =
                record.sharedMembers ? record.primaryIdentity : record.membership.identity;
            if (record.sharedMembers) {
                for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
                    if (const auto* identity = member_identity(record, row)) {
                        prepared.joinedIdentities[row] = *identity;
                    }
                }
            } else {
                prepared.joinedIdentities[0] = record.membership.identity;
            }
            prepared.targetSlot = slot;
            prepared.expectedStateRevision = activity.stateRevision;
            prepared.expectedRecordRevision = record.recordRevision;
            prepared.expectedCreatedRevision = record.createdRevision;
            prepared.membershipRevision = member->revision;
            prepared.peerTableEpoch = member->epoch;
            prepared.after = record.peerReservations;
            for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
                if (entity_slots::slot_count(record.memberLeases.held[row])) {
                    prepared.leasedRows |= 1U << row;
                }
            }
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return ready;
}

bool finish(PendingMutation& prepared, const Roster& before, Kind kind) noexcept {
    prepared.changed = prepared.after != before || prepared.releasedMemberRow < kInvalidMemberRow;
    if (prepared.changed) {
        if (prepared.membershipRevision == membership::kMaximumMembershipRevision) {
            return false;
        }
        ++prepared.membershipRevision;
    }
    for (std::size_t i = 0; i < prepared.after.peers.size(); ++i) {
        if (prepared.after.peers[i].memberKey != 0
            && !known(prepared.after.peers[i], prepared.accountGenerations[i])) {
            return false;
        }
    }
    if (prepared.after.primary.memberKey
        && !known(prepared.after.primary, prepared.primaryAccountGeneration)) {
        return false;
    }
    prepared.kind = kind;
    prepared.afterGuard = prepared.after;
    prepared.prepared = true;
    return true;
}
} // namespace

bool prepare_admit(std::uint64_t sessionId,
                   std::span<const membership::Identity> identities,
                   PendingMutation& mutation) noexcept {
    mutation = {};
    if (identities.size() > kPeerCapacity + 1) {
        return false;
    }
    PendingMutation prepared{};
    if (!prepare_base(sessionId, prepared)) {
        return false;
    }
    const auto before = prepared.after;
    for (const auto& identity : identities) {
        std::uint32_t ignored{};
        if (!known(identity, ignored)) {
            return false;
        }
        if (identity.accountSoid == prepared.publisherAccount) {
            if (identity.memberKey != prepared.publisherMemberKey) {
                return false;
            }
            continue;
        }
        if (identity.accountSoid == prepared.primaryIdentity.accountSoid) {
            if (prepared.joinedIdentities[0].memberKey) {
                if (identity.memberKey != prepared.primaryIdentity.memberKey
                    || identity.opaqueSoid != prepared.primaryIdentity.opaqueSoid) {
                    return false;
                }
            } else {
                auto& primary = prepared.after.primary;
                if ((primary.memberKey
                     && (primary.memberKey != identity.memberKey
                         || primary.accountSoid != identity.accountSoid
                         || primary.opaqueSoid != identity.opaqueSoid))
                    || identity.memberKey == prepared.publisherMemberKey
                    || std::any_of(prepared.after.peers.begin(),
                                   prepared.after.peers.end(),
                                   [&](const membership::Identity& peer) {
                                       return peer.memberKey == identity.memberKey;
                                   })) {
                    return false;
                }
                primary = identity;
            }
            continue;
        }
        if (identity.memberKey == prepared.primaryIdentity.memberKey) {
            return false;
        }
        if (identity.memberKey == prepared.after.primary.memberKey) {
            return false;
        }
        if (identity.memberKey == prepared.publisherMemberKey) {
            return false;
        }
        membership::Identity* selected{};
        for (auto& peer : prepared.after.peers) {
            if (peer.memberKey == identity.memberKey || peer.accountSoid == identity.accountSoid) {
                // A native rejoin must release its old reservation before changing machine keys.
                if (peer.memberKey != identity.memberKey || peer.accountSoid != identity.accountSoid
                    || peer.opaqueSoid != identity.opaqueSoid) {
                    return false;
                }
                selected = &peer;
                break;
            }
            if (peer.memberKey == 0 && selected == nullptr) {
                selected = &peer;
            }
        }
        if (!selected) {
            return false;
        }
        *selected = identity;
    }
    if (!finish(prepared, before, Kind::admit)) {
        return false;
    }
    mutation = prepared;
    return true;
}

bool prepare_release(std::uint64_t sessionId,
                     std::uint64_t memberKey,
                     PendingMutation& mutation,
                     ReleaseRefusal* refusal) noexcept {
    mutation = {};
    if (refusal) {
        *refusal = ReleaseRefusal::none;
    }
    PendingMutation prepared{};
    if (memberKey == 0 || !prepare_base(sessionId, prepared)
        || memberKey == prepared.publisherMemberKey) {
        return false;
    }
    const auto before = prepared.after;
    prepared.releasedMemberKey = memberKey;
    for (std::size_t row = 0; row < prepared.joinedIdentities.size(); ++row) {
        if (prepared.joinedIdentities[row].memberKey == memberKey) {
            prepared.releasedMemberRow = row;
        }
    }
    // A retract never unseats a live member. A departing client sends one retract per peer it is
    // dropping, and it arrives while those peers are still playing: taking the record's own
    // primary row, or any row still holding entity slots, evicts a player who never left. The
    // fork answers both cases without changing state and leaves the removal to `depart_member`,
    // the only path by which a member can leave its own row.
    if (prepared.releasedMemberRow == 0) {
        if (refusal) {
            *refusal = ReleaseRefusal::hostRow;
        }
        return false;
    }
    if (prepared.releasedMemberRow < kInvalidMemberRow
        && (prepared.leasedRows & (1U << prepared.releasedMemberRow)) != 0U) {
        if (refusal) {
            *refusal = ReleaseRefusal::leaseHeld;
        }
        return false;
    }
    for (auto& peer : prepared.after.peers) {
        if (peer.memberKey == memberKey) {
            peer = {};
        }
    }
    if (prepared.after.primary.memberKey == memberKey) {
        prepared.after.primary = {};
    }
    if (!finish(prepared, before, Kind::release)) {
        return false;
    }
    mutation = prepared;
    return true;
}

void invalidate_owner(std::uint64_t accountSoid) noexcept {
    if (!accountSoid) {
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    for (auto& record : activity.sessions) {
        if (!record.occupied || !record.joined || activity.stateRevision == kMaximumRevision) {
            continue;
        }
        const bool reserved =
            (record.peerReservations.primary.memberKey
             && record.peerReservations.primary.accountSoid == accountSoid)
            || std::any_of(record.peerReservations.peers.begin(),
                           record.peerReservations.peers.end(),
                           [=](const membership::Identity& peer) {
                               return peer.memberKey && peer.accountSoid == accountSoid;
                           });
        const auto ownRow =
            record.sharedMembers ? member_row(record, 0, accountSoid) : kInvalidMemberRow;
        if ((!reserved && ownRow == kInvalidMemberRow) || !can_republish_members(record, ownRow)) {
            continue;
        }
        bool hasRecipient{};
        for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
            const auto* member = member_state(record, row);
            hasRecipient |= row != ownRow && member && member->hasIdentity;
        }
        if (!hasRecipient) {
            continue;
        }
        republish_members(record, ownRow);
        record.recordRevision = ++activity.stateRevision;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

bool commit(PendingMutation& mutation) noexcept {
    const auto prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.kind == Kind::none || prepared.targetSlot >= kSessionCapacity
        || prepared.after != prepared.afterGuard || prepared.publisherAccount == 0
        || prepared.publisherAccount != account_primary_soid(bound_account())) {
        return false;
    }
    for (std::size_t i = 0; i < prepared.after.peers.size(); ++i) {
        if (prepared.after.peers[i].memberKey == 0) {
            continue;
        }
        std::uint32_t generation{};
        if (!known(prepared.after.peers[i], generation)
            || generation != prepared.accountGenerations[i]) {
            return false;
        }
    }
    if (prepared.after.primary.memberKey) {
        std::uint32_t generation{};
        if (!known(prepared.after.primary, generation)
            || generation != prepared.primaryAccountGeneration) {
            return false;
        }
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto& record = activity.sessions[prepared.targetSlot];
    auto* publisher = member_state(record, prepared.publisherRow);
    std::size_t releasedRow = kInvalidMemberRow;
    if (record.sharedMembers && prepared.kind == Kind::release) {
        for (std::size_t row = 0; row < kInvalidMemberRow; ++row) {
            const auto* identity = member_identity(record, row);
            if (identity && identity->memberKey == prepared.releasedMemberKey) {
                releasedRow = row;
            }
        }
    }
    auto expectedRelease = record.peerReservations;
    for (auto& peer : expectedRelease.peers) {
        if (peer.memberKey == prepared.releasedMemberKey) {
            peer = {};
        }
    }
    if (expectedRelease.primary.memberKey == prepared.releasedMemberKey) {
        expectedRelease.primary = {};
    }
    bool ready =
        record.occupied && record.joined && publisher && publisher->hasIdentity
        && record.sessionId == prepared.sessionId
        && record.createdRevision == prepared.expectedCreatedRevision
        && record.recordRevision == prepared.expectedRecordRevision
        && activity.stateRevision == prepared.expectedStateRevision
        && activity.stateRevision != kMaximumRevision
        && publisher->identity.memberKey == prepared.publisherMemberKey
        && publisher->identity.accountSoid == prepared.publisherAccount
        && publisher->epoch == prepared.peerTableEpoch && prepared.releasedMemberRow == releasedRow
        && (prepared.kind != Kind::release
            || (prepared.releasedMemberKey != 0
                && prepared.releasedMemberKey != prepared.publisherMemberKey && releasedRow != 0
                && (releasedRow >= kInvalidMemberRow
                    || !entity_slots::slot_count(record.memberLeases.held[releasedRow]))
                && prepared.after == expectedRelease))
        && (prepared.kind != Kind::admit || prepared.releasedMemberKey == 0)
        && (!prepared.changed || can_republish_members(record))
        && prepared.changed
               == (prepared.after != record.peerReservations || releasedRow < kInvalidMemberRow)
        && prepared.membershipRevision == publisher->revision + (prepared.changed ? 1U : 0U);
    auto fireteams = activity.fireteams;
    if (ready && prepared.kind == Kind::admit) {
        const auto& primary = prepared.after.primary;
        if (primary.memberKey) {
            ready = fireteams.establish(primary.accountSoid, prepared.publisherAccount);
        }
        for (const auto& peer : prepared.after.peers) {
            if (peer.memberKey != 0 && peer.accountSoid != prepared.publisherAccount
                && !fireteams.establish(peer.accountSoid, prepared.publisherAccount)) {
                ready = false;
                break;
            }
        }
    }
    if (ready && prepared.kind == Kind::admit && record.sharedMembers && member_joined(record, 0)
        && record.primaryIdentity.accountSoid != prepared.publisherAccount) {
        ready = fireteams.establish(record.primaryIdentity.accountSoid, prepared.publisherAccount);
    }
    if (ready && prepared.kind == Kind::release && prepared.changed) {
        const auto& primary = record.peerReservations.primary;
        if (primary.memberKey && !prepared.after.primary.memberKey) {
            static_cast<void>(fireteams.release(primary.accountSoid, prepared.publisherAccount));
        }
        if (const auto* released = member_identity(record, releasedRow)) {
            static_cast<void>(fireteams.release(released->accountSoid, prepared.publisherAccount));
        }
        for (const auto& peer : record.peerReservations.peers) {
            if (peer.memberKey != 0
                && std::none_of(prepared.after.peers.begin(),
                                prepared.after.peers.end(),
                                [&](const membership::Identity& kept) {
                                    return kept.memberKey == peer.memberKey;
                                })) {
                static_cast<void>(fireteams.release(peer.accountSoid, prepared.publisherAccount));
            }
        }
    }
    if (ready) {
        const bool groupChanged = fireteams.revision() != activity.fireteams.revision();
        activity.fireteams = fireteams;
        record.peerReservations = prepared.after;
        if (prepared.changed) {
            if (releasedRow < kInvalidMemberRow) {
                remove_joined_member(record, releasedRow);
            }
            publisher->revision = prepared.membershipRevision;
            publisher->acknowledgedRevision = membership::kAbsentRevision;
            republish_members(record, prepared.publisherRow);
        }
        if (prepared.changed || groupChanged) {
            ++activity.stateRevision;
            record.recordRevision = activity.stateRevision;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ready;
}
} // namespace sunrise::state::activity::reservations
