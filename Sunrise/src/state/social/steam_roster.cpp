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
    ++departing.publication;
    departing.deliveryConnection = 0;
    departing.deliverySerial = 0;
    chat_.disconnect(account);
    departing.inbox = {};
    departing.inboxCount = 0;
    for (auto& peer : accounts_) {
        for (std::size_t i = 0; i < peer.inboxCount;) {
            if (peer.inbox[i].inviterSoid == departing.row.primarySoid) {
                erase(peer.inbox, peer.inboxCount, i);
                ++peer.publication;
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
                ++peer.publication;
            } else {
                ++i;
            }
        }
    }
    // The publication counter outlives the identity it described: the guest compares it as a
    // high-water mark, and restarting it at zero would make a later feed look like an older one.
    const auto published = accounts_[account].publication;
    accounts_[account] = {};
    accounts_[account].publication = published + 1;
    chat_.forget(account);
    ++revision_;
    return true;
}

std::size_t Hub::link_count(AccountHandle account) const noexcept {
    return account < accounts_.size() ? accounts_[account].links : 0;
}

void Hub::delivery(AccountHandle account, std::uint32_t connection, std::uint64_t serial) noexcept {
    if (account >= accounts_.size()) {
        return;
    }
    // A later registration simply replaces the earlier one; late frames on the old connection are
    // consumed without changing current state because they no longer match this pair.
    accounts_[account].deliveryConnection = connection;
    accounts_[account].deliverySerial = serial;
}

bool Hub::delivers(AccountHandle account,
                   std::uint32_t connection,
                   std::uint64_t serial) const noexcept {
    return account < accounts_.size() && connection != 0 && serial != 0
           && accounts_[account].deliveryConnection == connection
           && accounts_[account].deliverySerial == serial;
}

Stamp Hub::stamp(AccountHandle account, std::uint32_t routeGeneration) const noexcept {
    Stamp value{};
    value.directory = revision_;
    value.routes = routeGeneration;
    if (account >= accounts_.size()) {
        return value;
    }
    value.account = accounts_[account].publication;
    value.lobby = chat_.publication(account);
    return value;
}

void Hub::release_waiters(const Account& target) noexcept {
    const auto primary = target.row.primarySoid;
    if (primary == 0) {
        return;
    }
    for (auto& peer : accounts_) {
        if (peer.waitingOn != primary) {
            continue;
        }
        peer.waitingOn = 0;
        ++peer.publication;
    }
}

bool Hub::apply(AccountHandle account, const feed::Sync& request) noexcept {
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
        source.receivedThrough = request.receivedThrough;
        // Replacing a registration rewrites the epoch, the cursors and the mailbox.
        ++source.publication;
    }
    const bool registering = !source.syncing;
    source.syncing = true;
    for (std::size_t i = 0; i < source.inboxCount;) {
        if (source.inbox[i].sequence <= request.receivedThrough) {
            erase(source.inbox, source.inboxCount, i);
            // The mailbox this account publishes just shrank.
            ++source.publication;
        } else {
            ++i;
        }
    }
    if (source.receivedThrough < request.receivedThrough) {
        source.receivedThrough = request.receivedThrough;
        ++source.publication;
    }
    if (registering || source.inboxCount != source.inbox.size()) {
        // Registration and mailbox room are the two conditions the accept loop breaks on, so a
        // sender parked on either is republished here rather than by a retry clock.
        release_waiters(source);
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
                source.waitingOn = target->row.primarySoid;
                break;
            }
            auto& delivery = target->inbox[target->inboxCount++];
            delivery = invite;
            delivery.sequence = target->nextDelivery++;
            // A new invitation is visible in the target's feed.
            ++target->publication;
        }
        source.acceptedThrough = invite.sequence;
        source.waitingOn = 0;
        // The operation this account asked for was accepted.
        ++source.publication;
    }
    chat_.sync(account, source.row.steamId, request.lobby);
    return true;
}

void Hub::publish(AccountHandle account, feed::Feed& output) const noexcept {
    output = {};
    if (account >= accounts_.size()) {
        return;
    }
    const auto& source = accounts_[account];
    output.epoch = source.epoch;
    output.acceptedThrough = source.acceptedThrough;
    output.publication = source.publication;
    output.receivedThrough = source.receivedThrough;
    for (AccountHandle i = 0; i < accounts_.size(); ++i) {
        if (i != account && accounts_[i].links != 0 && valid(accounts_[i].row)) {
            output.rows[output.rowCount++] = accounts_[i].row;
        }
    }
    output.invites = source.inbox;
    output.inviteCount = source.inboxCount;
    chat_.feed(account, output.lobby);
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
    incomingDeferred_ = false;
    // A fresh registration has shown the host nothing, so every retained operation is owed again.
    offeredThrough_ = 0;
    stagedThrough_ = 0;
    receiptSent_ = 0;
    knownPublication_ = 0;
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

void Client::snapshot(feed::Sync& output) noexcept {
    output = {};
    output.epoch = epoch_;
    output.acceptedThrough = acceptedThrough_;
    output.receivedThrough = receivedThrough_;
    output.invites = outgoing_;
    output.inviteCount = outgoingCount_;
    // Staged, not committed: only an accepted feed proves the host was shown this much.
    stagedThrough_ = outgoingCount_ != 0 ? outgoing_[outgoingCount_ - 1].sequence : offeredThrough_;
}

void Client::note_publication(std::uint64_t publication) noexcept {
    knownPublication_ = (std::max)(knownPublication_, publication);
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
    offeredThrough_ = (std::max)(offeredThrough_, stagedThrough_);
    // The host echoes its own receipt mark, so an unconfirmed receipt is the client's own work.
    receiptSent_ = (std::max)(receiptSent_, (std::min)(value.receivedThrough, receivedThrough_));
    knownPublication_ = (std::max)(knownPublication_, value.publication);
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
    incomingDeferred_ = false;
    for (std::size_t i = 0; i < value.inviteCount; ++i) {
        const auto& invite = value.invites[i];
        if (invite.sequence <= receivedThrough_) {
            continue;
        }
        if (incomingCount_ == incoming_.size()) {
            incomingDeferred_ = true;
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
bool pending_local_work() noexcept {
    {
        std::lock_guard lock(clientMutex);
        if (client.pending_local_work()) {
            return true;
        }
    }
    return lobby::dirty();
}
std::uint64_t known_publication() noexcept {
    std::lock_guard lock(clientMutex);
    return client.known_publication();
}
void note_publication(std::uint64_t publication) noexcept {
    std::lock_guard lock(clientMutex);
    client.note_publication(publication);
}
void reset_publication() noexcept {
    std::lock_guard lock(clientMutex);
    client.reset_publication();
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
