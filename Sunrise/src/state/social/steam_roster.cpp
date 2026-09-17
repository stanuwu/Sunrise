#include "steam_roster.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>

#include "../../core/threading/srw_lock.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../account/account_platform.h"

namespace sunrise::state::social {
namespace {
template <class T, std::size_t N>
void erase(std::array<T, N>& values, std::size_t& count, std::size_t index) noexcept {
    for (auto i = index + 1; i < count; ++i) {
        values[i - 1] = values[i];
    }
    values[--count] = {};
}
bool valid(const RosterEntry& row) noexcept {
    return row.primarySoid != 0 && account::platform::valid(row.steamId)
           && account::platform::declared_account_soid(row.steamId) == row.primarySoid
           && row.personaName.back() == '\0';
}
bool same(const RosterEntry& left, const RosterEntry& right) noexcept {
    return left.primarySoid == right.primarySoid && left.steamId == right.steamId
           && left.personaName == right.personaName;
}
bool valid(const Invite& invite) noexcept {
    return invite.sequence != 0 && invite.targetSoid != 0 && invite.inviterSoid != 0
           && invite.targetSoid != invite.inviterSoid && invite.connect.back() == '\0';
}
Hub directory;
Client client;
std::uint64_t localPrimary{};
// Release this mirror lock before entering lobby state or invoking client callbacks.
core::threading::SrwLock clientMutex;
} // namespace

void Hub::opened(AccountHandle account) noexcept {
    if (account >= accounts_.size()) {
        return;
    }
    if (accounts_[account].links++ == 0) {
        ++revision_;
    }
}

void Hub::closed(AccountHandle account) noexcept {
    if (account >= accounts_.size()) {
        return;
    }
    auto& departing = accounts_[account];
    if (departing.links == 0 || --departing.links != 0) {
        return;
    }
    departing.syncing = false;
    ++revision_;
    chat_.disconnect(account);
    departing.inbox = {};
    departing.inboxCount = 0;
    for (auto& peer : accounts_) {
        for (std::size_t i = 0; i < peer.inboxCount;) {
            if (peer.inbox[i].inviterSoid == departing.row.primarySoid) {
                erase(peer.inbox, peer.inboxCount, i);
            } else {
                ++i;
            }
        }
    }
    // Keep sequence high-water marks across reconnects so an uncertain send cannot duplicate.
}

bool Hub::publish(AccountHandle account, const RosterEntry& row) noexcept {
    if (account >= accounts_.size() || !valid(row)) {
        return false;
    }
    for (AccountHandle i = 0; i < accounts_.size(); ++i) {
        if (i != account
            && (accounts_[i].row.primarySoid == row.primarySoid
                || accounts_[i].row.steamId == row.steamId)) {
            return false;
        }
    }
    auto& stored = accounts_[account].row;
    if (stored.primarySoid != 0
        && (stored.primarySoid != row.primarySoid || stored.steamId != row.steamId)) {
        return false;
    }
    if (!same(stored, row)) {
        stored = row;
        ++revision_;
    }
    return true;
}

bool Hub::forget(AccountHandle account) noexcept {
    if (account == kLocalAccount || account >= accounts_.size() || accounts_[account].links != 0) {
        return false;
    }
    const auto primary = accounts_[account].row.primarySoid;
    for (auto& peer : accounts_) {
        for (std::size_t i = 0; i < peer.inboxCount;) {
            if (peer.inbox[i].inviterSoid == primary || peer.inbox[i].targetSoid == primary) {
                erase(peer.inbox, peer.inboxCount, i);
            } else {
                ++i;
            }
        }
    }
    accounts_[account] = {};
    chat_.forget(account);
    ++revision_;
    return true;
}

std::size_t Hub::link_count(AccountHandle account) const noexcept {
    return account < accounts_.size() ? accounts_[account].links : 0;
}

bool Hub::sync(AccountHandle account, const feed::Sync& request, feed::Feed& output) noexcept {
    if (account >= accounts_.size() || request.epoch == 0
        || request.inviteCount > request.invites.size()) {
        return false;
    }
    auto& source = accounts_[account];
    if (source.links == 0 || !valid(source.row)) {
        return false;
    }
    // Validate the whole request before touching any mailbox, including duplicates on a retry.
    std::uint64_t previous{};
    for (std::size_t i = 0; i < request.inviteCount; ++i) {
        const auto& invite = request.invites[i];
        if (!valid(invite) || invite.inviterSoid != source.row.primarySoid
            || invite.sequence <= previous) {
            return false;
        }
        previous = invite.sequence;
    }
    if (request.acceptedThrough == (std::numeric_limits<std::uint64_t>::max)()
        || request.receivedThrough == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (source.epoch == request.epoch
        && (request.receivedThrough >= source.nextDelivery
            || request.acceptedThrough > source.acceptedThrough)) {
        return false;
    }
    if (source.epoch == 0 && source.inboxCount != 0
        && request.receivedThrough >= source.nextDelivery) {
        return false;
    }
    if (source.epoch != request.epoch) {
        const bool firstEpoch = source.epoch == 0;
        source.epoch = request.epoch;
        source.acceptedThrough = request.acceptedThrough;
        if (!firstEpoch) {
            source.inbox = {};
            source.inboxCount = 0;
        }
        if (source.inboxCount == 0) {
            source.nextDelivery = request.receivedThrough + 1;
        }
    }
    source.syncing = true;
    for (std::size_t i = 0; i < source.inboxCount;) {
        if (source.inbox[i].sequence <= request.receivedThrough) {
            erase(source.inbox, source.inboxCount, i);
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < request.inviteCount; ++i) {
        const auto& invite = request.invites[i];
        if (invite.sequence <= source.acceptedThrough) {
            continue;
        }
        if (invite.sequence != source.acceptedThrough + 1) {
            break;
        }
        auto target = std::find_if(accounts_.begin(), accounts_.end(), [&](const Account& peer) {
            return peer.row.primarySoid == invite.targetSoid && peer.links != 0;
        });
        // A departed target withdraws the invitation. Mailbox pressure preserves it for retry.
        if (target != accounts_.end()) {
            if (!target->syncing || target->inboxCount == target->inbox.size()
                || target->nextDelivery == (std::numeric_limits<std::uint64_t>::max)()) {
                break;
            }
            auto& delivery = target->inbox[target->inboxCount++];
            delivery = invite;
            delivery.sequence = target->nextDelivery++;
        }
        source.acceptedThrough = invite.sequence;
    }
    output = {};
    output.epoch = source.epoch;
    output.acceptedThrough = source.acceptedThrough;
    output.revision = revision_;
    for (AccountHandle i = 0; i < accounts_.size(); ++i) {
        if (i != account && accounts_[i].links != 0 && valid(accounts_[i].row)) {
            output.rows[output.rowCount++] = accounts_[i].row;
        }
    }
    output.invites = source.inbox;
    output.inviteCount = source.inboxCount;
    chat_.sync(account, source.row.steamId, request.lobby);
    chat_.feed(account, output.lobby);
    return true;
}

void Client::initialize(std::uint64_t primarySoid, std::uint64_t epoch) noexcept {
    std::destroy_at(this);
    std::construct_at(this);
    primarySoid_ = primarySoid;
    epoch_ = epoch;
}

void Client::disconnected() noexcept {
    if (rowCount_ != 0) {
        ++revision_;
    }
    rows_ = {};
    rowCount_ = 0;
    incoming_ = {};
    incomingCount_ = 0;
}

bool Client::post(const Invite& invite) noexcept {
    if (primarySoid_ == 0 || epoch_ == 0 || invite.inviterSoid != primarySoid_
        || invite.targetSoid == primarySoid_ || invite.connect.back() != '\0'
        || outgoingCount_ == outgoing_.size()
        || nextSequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (std::none_of(
            rows_.begin(),
            rows_.begin() + static_cast<std::ptrdiff_t>(rowCount_),
            [&](const RosterEntry& row) { return row.primarySoid == invite.targetSoid; })) {
        return false;
    }
    outgoing_[outgoingCount_] = invite;
    outgoing_[outgoingCount_++].sequence = nextSequence_++;
    return true;
}

void Client::snapshot(feed::Sync& output) const noexcept {
    output = {};
    output.epoch = epoch_;
    output.acceptedThrough = acceptedThrough_;
    output.receivedThrough = receivedThrough_;
    output.invites = outgoing_;
    output.inviteCount = outgoingCount_;
}

bool Client::receive(const feed::Feed& value) noexcept {
    if (epoch_ == 0 || value.epoch != epoch_ || value.rowCount > rows_.size()
        || value.inviteCount > incoming_.size() || value.acceptedThrough >= nextSequence_
        || value.acceptedThrough < acceptedThrough_) {
        return false;
    }
    for (std::size_t i = 0; i < value.rowCount; ++i) {
        const auto& row = value.rows[i];
        if (!valid(row) || row.primarySoid == primarySoid_) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (row.primarySoid == value.rows[j].primarySoid
                || row.steamId == value.rows[j].steamId) {
                return false;
            }
        }
    }
    std::uint64_t previous{};
    for (std::size_t i = 0; i < value.inviteCount; ++i) {
        const auto& invite = value.invites[i];
        if (!valid(invite) || invite.targetSoid != primarySoid_ || invite.sequence <= previous
            || std::none_of(
                value.rows.begin(),
                value.rows.begin() + static_cast<std::ptrdiff_t>(value.rowCount),
                [&](const RosterEntry& row) { return row.primarySoid == invite.inviterSoid; })) {
            return false;
        }
        previous = invite.sequence;
    }
    bool changed = value.rowCount != rowCount_;
    for (std::size_t i = 0; i < value.rowCount && !changed; ++i) {
        changed = !same(value.rows[i], rows_[i]);
    }
    rows_ = value.rows;
    rowCount_ = value.rowCount;
    if (changed) {
        ++revision_;
    }
    acceptedThrough_ = value.acceptedThrough;
    while (outgoingCount_ != 0 && outgoing_[0].sequence <= acceptedThrough_) {
        erase(outgoing_, outgoingCount_, 0);
    }
    for (std::size_t i = 0; i < incomingCount_;) {
        if (std::none_of(rows_.begin(),
                         rows_.begin() + static_cast<std::ptrdiff_t>(rowCount_),
                         [&](const RosterEntry& row) {
                             return row.primarySoid == incoming_[i].inviterSoid;
                         })) {
            erase(incoming_, incomingCount_, i);
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < value.inviteCount; ++i) {
        const auto& invite = value.invites[i];
        if (invite.sequence <= receivedThrough_) {
            continue;
        }
        if (incomingCount_ == incoming_.size()) {
            break;
        }
        incoming_[incomingCount_++] = invite;
        receivedThrough_ = invite.sequence;
    }
    return true;
}

bool Client::take(Invite& output) noexcept {
    if (incomingCount_ == 0) {
        return false;
    }
    output = incoming_[0];
    erase(incoming_, incomingCount_, 0);
    return true;
}

std::size_t Client::peers(std::span<RosterEntry> output) const noexcept {
    const auto count = (std::min)(output.size(), rowCount_);
    std::copy_n(rows_.begin(), count, output.begin());
    return count;
}

Hub& session_directory() noexcept {
    return directory;
}
void reset_directory() noexcept {
    std::destroy_at(&directory);
    std::construct_at(&directory);
}
bool initialize_client(std::uint64_t primarySoid) noexcept {
    std::lock_guard lock(clientMutex);
    if (primarySoid == 0) {
        return false;
    }
    if (primarySoid == localPrimary) {
        return true;
    }
    std::uint64_t epoch{};
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&epoch, 1)))
        || epoch == 0) {
        return false;
    }
    client.initialize(primarySoid, epoch);
    localPrimary = primarySoid;
    return true;
}
void client_disconnected() noexcept {
    std::lock_guard lock(clientMutex);
    client.disconnected();
}
void snapshot_sync(feed::Sync& output) noexcept {
    {
        std::lock_guard lock(clientMutex);
        client.snapshot(output);
    }
    lobby::snapshot(output.lobby);
}
bool apply_feed(const feed::Feed& value) noexcept {
    {
        std::lock_guard lock(clientMutex);
        if (!client.receive(value)) {
            return false;
        }
    }
    lobby::receive(value.lobby);
    return true;
}
bool post_invite(const Invite& value) noexcept {
    std::lock_guard lock(clientMutex);
    return client.post(value);
}
bool take_invite(std::uint64_t targetSoid, Invite& output) noexcept {
    std::lock_guard lock(clientMutex);
    return targetSoid == localPrimary && client.take(output);
}
std::size_t snapshot_peers(AccountHandle viewer, std::span<RosterEntry> output) noexcept {
    std::lock_guard lock(clientMutex);
    return viewer == kLocalAccount ? client.peers(output) : 0;
}
std::uint64_t revision() noexcept {
    std::lock_guard lock(clientMutex);
    return client.revision();
}
bool dirty() noexcept {
    std::lock_guard lock(clientMutex);
    return client.dirty();
}
std::uint64_t platform_id_for_soid(std::uint64_t soid) noexcept {
    std::array<RosterEntry, kRosterCapacity> peers{};
    const auto count = snapshot_peers(kLocalAccount, peers);
    for (std::size_t i = 0; i < count; ++i) {
        if (peers[i].primarySoid == soid) {
            return peers[i].steamId;
        }
    }
    return 0;
}
std::uint64_t soid_for_steam_id(std::uint64_t platformId) noexcept {
    std::array<RosterEntry, kRosterCapacity> peers{};
    const auto count = snapshot_peers(kLocalAccount, peers);
    for (std::size_t i = 0; i < count; ++i) {
        if (peers[i].steamId == platformId) {
            return peers[i].primarySoid;
        }
    }
    return 0;
}

} // namespace sunrise::state::social
